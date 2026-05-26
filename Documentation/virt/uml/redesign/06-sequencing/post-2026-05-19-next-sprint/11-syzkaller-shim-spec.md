# 11 — syzkaller `vm/uml` shim spec (Memo 09 Phase 4)

Status: design memo, no code yet.  Author: 2026-05-21.

Reads-on: Memo 09 (`09-fork-server-STATUS.md`, ...), the just-landed
`umlctl pool serve` daemon (`tools/uml/uml-launcher/src/bin/umlctl/
pool_serve.rs`, commit `5576cdf21084`).

Purpose: pin down exactly what the new syzkaller backend
(`vm/uml/uml.go`, upstream — or in our fork) has to do, and what
host-side `umlctl` subcommands the spec needs that aren't yet in
tree.  The implementation session reading this memo should be able
to start coding without ambiguity.

Sources (WebFetch, 2026-05-21):

  * `github.com/google/syzkaller/vm/vmimpl/vmimpl.go`  — Pool /
    Instance interfaces, Env / BootError / InfraError, `Register`,
    `Type`, `ctorFunc`.
  * `github.com/google/syzkaller/vm/vmimpl/util.go`     — SSHOptions
    / SCPOptions / SSH helpers.
  * `github.com/google/syzkaller/vm/vmimpl/merger.go`   — Chunk /
    OutputMerger.
  * `github.com/google/syzkaller/vm/qemu/qemu.go`       — reference
    backend that uses SSH+SCP+QMP.
  * `github.com/google/syzkaller/vm/gvisor/gvisor.go`   — reference
    backend that does NOT use SSH (Run = `runsc exec`; Copy = file
    drop into image dir).
  * `github.com/google/syzkaller/vm/isolated/isolated.go` — reference
    backend for pre-existing already-booted machines.
  * `github.com/google/syzkaller/vm/vm.go`              — backend
    registry (blank-import + `vmimpl.Register`).

WebFetch reached all of the above; no upstream signature was
inferred or invented.  Line numbers vary across upstream commits and
are deliberately omitted in favour of file:identifier references the
reader can grep on a fresh checkout.

---

## 1.  syzkaller's `vm/` interface surface

The total surface a backend must implement is two Go interfaces
plus one `init()`-time registration.  This is the entire contract.

### 1.1  `vmimpl.Pool` (`vm/vmimpl/vmimpl.go`)

```go
type Pool interface {
    Count() int
    Create(ctx context.Context, workdir string, index int) (Instance, error)
}
```

  * `Count()` is the static fleet size.  syzkaller calls it once at
    startup, after the ctor has returned, to size its scheduler.
  * `Create(ctx, workdir, index)` is invoked once per slot at
    startup AND on every crash/respawn.  `index` is in `[0, Count())`.
    `workdir` is a per-instance scratch directory syzkaller pre-
    creates; the backend may stash logs / a unix socket / a serial
    pipe under it.  The function must block until the VM is
    reachable for `Run()`, OR return a `*vmimpl.BootError` (transient
    boot failure, retried) or `*vmimpl.InfraError` (infrastructure,
    retried more conservatively).  Any other error is fatal for
    that slot.

### 1.2  `vmimpl.Instance` (`vm/vmimpl/vmimpl.go`)

```go
type Instance interface {
    Copy(hostSrc string) (string, error)
    Forward(port int) (string, error)
    Run(ctx context.Context, command string) (<-chan Chunk, <-chan error, error)
    Diagnose(rep *report.Report) (diagnosis []byte, wait bool)
    io.Closer  // Close() error
}
```

Optional but ubiquitously implemented:

```go
Info() ([]byte, error)
```

  * `Copy(hostSrc)` puts a host file into the guest, returns the
    guest path.  qemu does `scp`; gvisor copies into the rootfs
    image directory; isolated does `scp` to a pre-booted machine.
    For UML with hostfs root, `Copy` is a no-op rename inside a
    shared directory — see §4.
  * `Forward(port)` configures a way for code running INSIDE the
    guest to dial back out to the host on `port`.  Returns the
    address the guest should use (e.g. `"10.0.2.10:35419"` for
    qemu, `"stdin:0"` for gvisor, `"<host-ip>:<port>"` for
    isolated).  Called once per instance, BEFORE `Run`.
  * `Run(ctx, command)` is the workhorse.  It must:
      1. ship `command` (a shell-ish line, e.g.
         `/syz-executor exec ...`) to the guest and start it;
      2. multiplex the resulting stdout+stderr into a `chan Chunk`
         (typed `vmimpl.StdoutOutput` / `vmimpl.StderrOutput` /
         `vmimpl.ConsoleOutput`);
      3. signal completion on a `chan error` (nil for clean exit,
         non-nil for any failure including non-zero exit);
      4. honour `ctx` cancellation by killing the in-guest command
         and tearing down the channels.
  * `Diagnose(rep)` is best-effort debug capture after a crash.
    qemu pokes QMP for register dumps; gvisor catenates the sandbox
    logs.  Returns (a blob, `wait bool`) — if `wait` is true,
    syzkaller pauses before destroying the instance so an external
    operator can inspect.
  * `Close()` destroys the instance.  Must be idempotent; syzkaller
    sometimes calls it twice on shutdown paths.
  * `Info()` is metadata for the report header — qemu returns the
    qemu version + argv; we'll return `umlctl --version` plus the
    pool name and master pid.

### 1.3  `Chunk` and `OutputMerger` (`vm/vmimpl/merger.go`)

```go
type Chunk struct {
    Data []byte
    Type OutputType
}

type OutputMerger struct {
    Output chan Chunk
    // …
}
```

`OutputMerger` is a utility most backends use to fold several
io.Readers (kernel console, ssh stdout, ssh stderr) into one
`chan Chunk` of typed lines.  We'll use it the same way.

### 1.4  Registration (`vm/vmimpl/vmimpl.go`, `vm/vm.go`)

```go
type ctorFunc func(env *Env) (Pool, error)

type Type struct {
    Ctor        ctorFunc
    Overcommit  bool
    Preemptible bool
}

var Types = make(map[string]Type)

func Register(typ string, desc Type) { Types[typ] = desc }
```

A backend lives in its own package and uses `init()`:

```go
func init() {
    vmimpl.Register("uml", vmimpl.Type{
        Ctor:        ctor,
        Overcommit:  true,  // UML processes overcommit fine
        Preemptible: true,  // SIGKILL the host pid is safe
    })
}
```

The blank import line goes into `vm/vm.go` (`_ "github.com/google/
syzkaller/vm/uml"`).  That is the entire upstream hook.

### 1.5  `vmimpl.Env`

`Env` carries: `Name, OS, Arch, Workdir, Image, SSHKey, SSHUser,
Timeouts, Snapshot, Debug, Config, KernelSrc`.

  * `Config` is a `json.RawMessage` — backends decode their own
    config struct from it.  Our struct is sketched in §3.7.
  * `Image` and `SSHKey`/`SSHUser` are unused by the UML backend
    (we run hostfs, not a disk image, and we don't go through SSH).
  * `Workdir` is the per-pool workdir, distinct from the per-
    instance `workdir` passed to `Create`.

---

## 2.  The shim's responsibilities (1-page summary)

The shim (`vm/uml/uml.go`, ~150 LoC Go) is glue, not logic.  All
the heavy lifting — boot, identity, networking, teardown — lives
inside the in-tree Rust binary `umlctl` (`tools/uml/uml-launcher`).
The shim's only jobs are:

  1. **Discover or start a pool daemon.**  On `ctor(env)`, locate
     a running `umlctl pool serve --name <pool>` daemon by looking
     up its socket at `$XDG_RUNTIME_DIR/uml/pools/<pool>/api.sock`.
     If absent, optionally start it (config knob, `auto_serve:
     true`).
  2. **Convert a `Create()` call into a `take` RPC.**  Pick a MAC /
     TAP / IPv4 from the per-instance config or auto-generate them
     from `index`.  Send `{"op":"take", "instance":..., "mac":...,
     "tap":..., "ipv4":..., "gateway":..., "mconsole":""}` to the
     pool socket.  The reply is a `SpawnResult` (pid + bookkeeping)
     — see `pool_serve.rs`.  Stash it in the per-instance struct.
  3. **Convert a `Run()` call into `umlctl exec`.**  Shell out to
     `umlctl exec --pid <pid> --json --timeout <t> --command <cmd>`
     and stream its stdout/stderr through an `OutputMerger`.
  4. **Convert a `Copy()` call into a hostfs path translation.**
     UML's root is hostfs, so the host path IS visible from the
     guest at a deterministic location (`/host/<abs-path>` or a
     bind-mounted scratch dir).  Returns the guest-visible string;
     no `scp` needed.  See §3 and §4 for the `port-forward`-vs-
     `tap` trade-off.
  5. **Convert a `Forward()` call into a no-op.**  Because the
     guest already has an IPv4 on a host-visible TAP (set up by
     the pool daemon when the member was taken), the guest can
     dial the host's TAP gateway IP directly.  Return that
     address.  No `umlctl port-forward` is strictly required if we
     accept this trade-off; §4 weighs it explicitly.
  6. **Convert a `Close()` call into `umlctl pool destroy`.**
     `{"op":"destroy", "pid":<pid>}` via the same daemon socket.
     Daemon does SIGKILL + zombie reap.
  7. **`Diagnose` and `Info`.**  `Diagnose` reads the per-instance
     `workdir/kernel.log` plus the daemon's `pool status` reply.
     `Info` returns a short banner: `umlctl <git-sha> pool=<name>
     master_pid=<n>`.

Out of scope for the shim (handled in tree by `umlctl`):

  * boot / template-pause / fork-on-resume bookkeeping;
  * TAP / IPv4 / iptables / NAT;
  * cgroup setup;
  * crash detection (parsed from the kernel log by `umlctl`'s
    existing dmesg parser + events spine).

---

## 3.  Required new `umlctl` subcommands

Two new top-level verbs.  Each is sized to the shim's actual need;
we explicitly resist adding knobs that no caller will use.

### 3.1  `umlctl exec` — run a command in a running pool member

Today the daemon manages lifecycle (take / list / destroy) but
gives the caller no way to push a command into the member's user-
space.  syzkaller's `Run()` REQUIRES this.

**Why a new verb and not reuse `mission` / `gate loop`.**  Those
verbs run a whole fresh kernel.  `exec` runs INSIDE a member that
is already booted and identity-plumbed — its purpose is to run
syz-executor (or any test binary) and return its output.

#### 3.1.1  CLI shape

```
umlctl exec --pid <PID> [--timeout SECS] [--json]
            [--stdin-file PATH] [--cwd PATH] [--env K=V]…
            -- COMMAND [ARGS…]
```

  * `--pid <PID>` is required and is the host pid of the pool
    member as returned by `pool spawn` / `pool serve` take.
  * `--timeout SECS` (default 0 = no timeout) bounds wall time;
    on expiry the in-guest command is killed and exec exits 124.
  * `--json` switches stdout to one NDJSON object per line; without
    it, the verb is a transparent stdin/stdout/stderr pass-through
    so an operator can `umlctl exec --pid 1234 -- /bin/sh -i`.
  * Positional `--` is the command argv handed to the guest.

#### 3.1.2  Transport — pty over the daemon socket

The simplest implementation: extend the existing pool daemon with
one more RPC.

```
--> {"op":"exec", "pid":N, "argv":["…","…"],
     "env":{"K":"V",…}, "cwd":"/…", "timeout_secs":T}
<-- {"ok":true, "stream_fd":N}      // SCM_RIGHTS-passed pty master
```

The daemon, which already shares the cgroup / namespace family
with the member (it is the member's parent), allocates a pty pair,
forks an `execveat` into the member via the mconsole `exec`
command (mconsole is already wired per `SpawnArgs::mconsole`), and
returns the pty master fd via SCM_RIGHTS on the same Unix socket.

`umlctl exec` then `splice(2)`s stdin / stdout / stderr to/from the
pty until EOF or the timeout fires, parses the in-guest exit code
out of the last line (`__UMLCTL_EXIT__ <n>\n` written by a tiny
wrapper script in the guest), and exits with the same code.

If `--json` is set, output is shaped as:

```
{"type":"stdout","data":"…"}
{"type":"stderr","data":"…"}
{"type":"exit","code":N,"signal":S,"duration_ms":D}
```

This is the shape `vm/uml/uml.go` consumes directly.

#### 3.1.3  Why pty and not a plain socket

  * syzkaller's manager assumes the guest can flush stdout
    line-by-line (it parses progress lines live).  A pipe with
    8 KiB pipe buffering and no PTY-style line discipline can
    delay output across whole batches of fuzz programs.  A pty
    master gives us per-line flushing for free.
  * A pty also gives us `TIOCGWINSZ` for an operator running
    `umlctl exec -- /bin/sh -i`, which makes the verb useful
    outside the syzkaller use-case.

#### 3.1.4  Failure modes

  * No such pid → exit 2 + `{"ok":false,"error":"no such pid"}`.
  * Member's mconsole socket missing → exit 3, "member has no
    mconsole; respawn with --mconsole".
  * Timeout → exit 124 + final `{"type":"exit","code":124,
    "signal":9,"duration_ms":…}`.
  * Daemon down → exit 4, "no pool daemon at <socket>; start
    one with `umlctl pool serve`".

### 3.2  `umlctl port-forward` — guest-to-host port reachability

Strictly OPTIONAL for the syzkaller integration if §4's TAP trade-
off is acceptable.  Documented here for completeness because the
Memo 09 Phase 4 budget mentions it.

#### 3.2.1  CLI shape

```
umlctl port-forward --pid <PID> --host-port <HPORT>
                    [--guest-port <GPORT>] [--json]
```

Returns:

```
{"ok":true, "guest_address":"<addr>:<port>"}
```

The guest-side address is suitable for the test binary inside the
guest to dial back to the host on `HPORT`.  Two implementation
options:

  * **TAP-only (preferred).**  The host's TAP gateway IP
    (`SpawnResult.ipv4_gateway`) is already reachable from the
    guest the moment the member is taken — that's literally what
    the per-member TAP+IPv4 wiring buys us.  `port-forward` then
    becomes a JSON-emitting query: return `<gateway-ip>:<hport>`
    without touching any kernel state.
  * **iptables DNAT (fallback).**  If the operator's host firewall
    blocks gateway-IP-bound dials from the guest, install a DNAT
    rule on `tap-<instance>` and undo it on `--remove` (a second
    verb form).  We defer this complexity until a real caller
    needs it.

The shim does NOT need this verb in the TAP-only world.  We keep
the verb proposal on file so that, if a future syzkaller feature
(e.g. the dashboard-proxy plumbing) demands a real port-forward,
we don't have to invent it under deadline.

### 3.3  Minor follow-on verbs the shim wants

  * `umlctl pool take`.  Today only `pool serve` (the daemon side)
    speaks the `take` RPC; there is no client-side verb.  The
    shim could either (a) speak the JSON protocol itself in Go, or
    (b) we add `umlctl pool take --name <pool> [--instance …] [--mac …]
    [--tap …] [--ipv4 …] [--gateway …] [--mconsole …] --json` that
    forwards to the daemon and prints the `SpawnResult`.  (b) is ~30
    LoC of Rust and makes the shim ~10 LoC of `os/exec` instead of
    ~80 LoC of net.Dial + json.Encoder/Decoder.  Recommend (b).
  * `umlctl pool status --name <pool> --json`.  Wraps the daemon's
    `{"op":"status"}` RPC.  `vm/uml/uml.go::Info` calls it.
  * `umlctl pool destroy --pid N --name <pool>`.  Wraps the
    daemon's `{"op":"destroy"}` RPC.  Note the existing `umlctl
    pool destroy` verb operates on the daemon-less spawn-record
    files (`$RUNTIME_DIR/pools/members/*.json`); the daemon-aware
    flavor needs `--name <pool>` to route through the socket.  Can
    be expressed as a flag on the existing verb rather than a new
    verb.

### 3.4  JSON output schema — `umlctl exec --json` (canonical)

```jsonc
// Header (first line, always present)
{"type":"start", "pid":N, "argv":[…], "cwd":"…", "ts_ns":1234567890}

// Output frames (zero or more, interleaved)
{"type":"stdout", "data":"…raw bytes utf-8-lossy…"}
{"type":"stderr", "data":"…"}

// Optional in-guest console capture (kernel printk emitted DURING
// the exec window).  Lets syzkaller-side crash-detection see oopses
// without a separate file-tail.
{"type":"console", "data":"…"}

// Final frame (always present, exactly one)
{"type":"exit", "code":N, "signal":S, "duration_ms":D,
 "timed_out":false}
```

  * `data` is a UTF-8 string; bytes that don't decode are passed
    through with Rust's `String::from_utf8_lossy` (one `U+FFFD`
    per invalid byte).
  * No frame is ever larger than 64 KiB; long lines are split.
  * `exit` is the LAST frame; readers can stop on it.

### 3.5  JSON output schema — `umlctl port-forward --json`

```jsonc
{"ok":true, "guest_address":"10.7.0.1:35419",
 "scheme":"tap-direct",   // or "iptables-dnat"
 "host_port":35419, "guest_port":35419, "pid":N}
```

`scheme` is informational; the shim doesn't branch on it.

### 3.6  Schema versioning

Add a `"schema_version":"exec/1"` (or `"port-forward/1"`) field to
the `start` and `port-forward` headers.  Bumping is cheap; making
syzkaller tolerant of an unknown bump from day 0 saves a future
breaking change.

### 3.7  syzkaller-side config block

`vm/uml/uml.go`'s `Config` struct, JSON-decoded from `Env.Config`:

```go
type Config struct {
    Pool      string `json:"pool"`         // "default"
    UmlctlBin string `json:"umlctl"`       // "/usr/local/bin/umlctl"
    AutoServe bool   `json:"auto_serve"`   // start `pool serve` if absent
    Kernel    string `json:"kernel"`       // path to fork-capable vmlinux
    Count     int    `json:"count"`        // matches syzkaller's vm count
    CpuShare  string `json:"cpu_share"`    // optional cgroup hint
    MemMB     int    `json:"mem_mb"`       // matches umlctl's --mem
    TapPrefix string `json:"tap_prefix"`   // "tap-uml-"
    Subnet    string `json:"subnet"`       // "10.7.0.0/24"
}
```

`Count` must match the upper `vm.count` in syzkaller's main config;
the shim cross-checks and returns a clear error if not.

---

## 4.  Optional `umlctl` features that would simplify the shim

Each item below would remove logic from `vm/uml/uml.go`.  None is
required for Phase 4; ranked roughly by ROI.

  1. **`umlctl pool take` client verb.**  Already discussed in §3.3.
     Cuts the shim by ~50 LoC of socket plumbing.
  2. **`umlctl exec --json` framing (§3.4).**  The shim must
     otherwise reimplement line-buffered stdout/stderr separation
     plus a private exit-code protocol.  With NDJSON, the shim is
     a `json.Decoder` loop.
  3. **`umlctl pool status --name <pool> --json`.**  Lets `Info()`
     be one shell-out plus one struct decode.
  4. **`umlctl pool serve --notify-ready`.**  Today the daemon
     bookkeeps a pidfile but does not signal readiness; the shim's
     `auto_serve` path must poll the socket for ~1 s before the
     first `take`.  Adding an sd_notify-style READY=1 (or `--wait-
     for-listen`) would let the shim block on a single fd.
  5. **Per-instance crash log path in `SpawnResult`.**  The take
     reply already returns `instance` / `mac` / `tap`; adding
     `console_log:"<workdir>/<instance>.console"` would let the
     shim tail one file per member for `Diagnose`.  Trivial daemon
     change.
  6. **`umlctl exec --capture-console`.**  If `exec` could
     piggy-back the kernel console capture for the duration of the
     command and inline it as `{"type":"console", …}` frames, the
     shim doesn't have to merge two independent streams.
  7. **`umlctl port-forward` even in TAP-only mode.**  Just to
     give the shim ONE call shape — `port-forward` returns the
     gateway-IP address; the shim doesn't have to branch on
     "is this TAP-direct or DNAT".

---

## 5.  ~150 LoC Go pseudocode for `vm/uml/uml.go`

This is illustrative, not production Go — but every method body
maps 1:1 to the production version.  The line budget (`~150`) is
the Memo 09 Phase 4 target.

```go
// vm/uml/uml.go
//
// SPDX-License-Identifier: Apache-2.0
//
// syzkaller backend for User-Mode Linux fork-server pools.
// All real work lives in umlctl (in-tree Rust binary).  This file
// is glue: shell out, JSON-decode, stream.

package uml

import (
    "context"
    "encoding/json"
    "fmt"
    "io"
    "os/exec"
    "path/filepath"
    "strings"
    "time"

    "github.com/google/syzkaller/pkg/report"
    "github.com/google/syzkaller/vm/vmimpl"
)

type Config struct {
    Pool      string `json:"pool"`
    UmlctlBin string `json:"umlctl"`
    AutoServe bool   `json:"auto_serve"`
    Kernel    string `json:"kernel"`
    Count     int    `json:"count"`
    MemMB     int    `json:"mem_mb"`
    TapPrefix string `json:"tap_prefix"`
    Subnet    string `json:"subnet"`
}

type pool struct {
    env *vmimpl.Env
    cfg *Config
}

type instance struct {
    pool        *pool
    index       int
    pid         int
    instance    string  // logical instance name (e.g. "uml-0")
    workdir     string
    mac, tap    string
    ipv4, gw    string
    consoleLog  string
    forwardPort int
    merger      *vmimpl.OutputMerger
    closed      bool
}

func init() {
    vmimpl.Register("uml", vmimpl.Type{
        Ctor:        ctor,
        Overcommit:  true,
        Preemptible: true,
    })
}

func ctor(env *vmimpl.Env) (vmimpl.Pool, error) {
    cfg := &Config{Pool: "default", UmlctlBin: "umlctl",
                   TapPrefix: "tap-uml-", Subnet: "10.7.0.0/24"}
    if err := json.Unmarshal(env.Config, cfg); err != nil {
        return nil, fmt.Errorf("uml: bad config: %w", err)
    }
    if cfg.AutoServe {
        // best-effort; ignore "already running" by exit code 0/17
        _ = exec.Command(cfg.UmlctlBin, "pool", "serve",
            "--name", cfg.Pool, "--kernel", cfg.Kernel,
            "--background").Run()
    }
    return &pool{env: env, cfg: cfg}, nil
}

func (p *pool) Count() int { return p.cfg.Count }

func (p *pool) Create(ctx context.Context, workdir string, index int) (vmimpl.Instance, error) {
    name := fmt.Sprintf("uml-%d", index)
    tap := p.cfg.TapPrefix + name
    ipv4 := allocIPv4(p.cfg.Subnet, index) // helper, ~5 LoC
    mac := allocMAC(index)                 // helper, ~3 LoC
    out, err := exec.CommandContext(ctx, p.cfg.UmlctlBin,
        "pool", "take", "--name", p.cfg.Pool, "--json",
        "--instance", name, "--mac", mac, "--tap", tap,
        "--ipv4", ipv4.CIDR, "--gateway", ipv4.Gateway).Output()
    if err != nil {
        return nil, &vmimpl.BootError{Title: "umlctl pool take failed",
                                      Output: out}
    }
    var sr struct {
        Pid         int    `json:"pid"`
        ConsoleLog  string `json:"console_log"` // optional, see §4 item 5
    }
    if err := json.Unmarshal(out, &sr); err != nil {
        return nil, fmt.Errorf("uml: parse take reply: %w", err)
    }
    return &instance{pool: p, index: index, pid: sr.Pid,
        instance: name, workdir: workdir, mac: mac, tap: tap,
        ipv4: ipv4.CIDR, gw: ipv4.Gateway,
        consoleLog: sr.ConsoleLog}, nil
}

func (inst *instance) Copy(hostSrc string) (string, error) {
    // hostfs root: the host path is already visible from the guest.
    // umlctl mounts the host root at /host inside the guest by
    // default; if a per-instance scratch bind is configured, prefer
    // it.  Either way: filepath.Join("/host", abs(hostSrc)).
    abs, err := filepath.Abs(hostSrc)
    if err != nil { return "", err }
    return filepath.Join("/host", abs), nil
}

func (inst *instance) Forward(port int) (string, error) {
    inst.forwardPort = port
    // Guest can dial the host's TAP gateway IP directly.  No
    // iptables, no umlctl port-forward needed for the common case.
    host := strings.TrimSuffix(strings.SplitN(inst.gw, "/", 2)[0], "/")
    return fmt.Sprintf("%s:%d", host, port), nil
}

func (inst *instance) Run(ctx context.Context, command string) (
    <-chan []byte, <-chan error, error) {
    args := []string{"exec", "--pid", fmt.Sprint(inst.pid), "--json",
        "--", "/bin/sh", "-c", command}
    cmd := exec.CommandContext(ctx, inst.pool.cfg.UmlctlBin, args...)
    stdout, _ := cmd.StdoutPipe()
    if err := cmd.Start(); err != nil { return nil, nil, err }
    outCh := make(chan vmimpl.Chunk, 64)
    errCh := make(chan error, 1)
    go func() {
        defer close(outCh)
        dec := json.NewDecoder(stdout)
        for {
            var frame struct {
                Type string `json:"type"`
                Data string `json:"data"`
                Code int    `json:"code"`
            }
            if err := dec.Decode(&frame); err != nil {
                if err == io.EOF { break }
                errCh <- err; return
            }
            switch frame.Type {
            case "stdout":  outCh <- vmimpl.Chunk{
                Data: []byte(frame.Data), Type: vmimpl.StdoutOutput}
            case "stderr":  outCh <- vmimpl.Chunk{
                Data: []byte(frame.Data), Type: vmimpl.StderrOutput}
            case "console": outCh <- vmimpl.Chunk{
                Data: []byte(frame.Data), Type: vmimpl.ConsoleOutput}
            case "exit":
                if frame.Code != 0 {
                    errCh <- fmt.Errorf("exit %d", frame.Code)
                } else { errCh <- nil }
                return
            }
        }
        errCh <- cmd.Wait()
    }()
    return outCh, errCh, nil
}

func (inst *instance) Diagnose(rep *report.Report) ([]byte, bool) {
    if inst.consoleLog == "" { return nil, false }
    data, _ := exec.Command(inst.pool.cfg.UmlctlBin,
        "pool", "status", "--name", inst.pool.cfg.Pool, "--json").Output()
    return data, false // wait=false; we have the log already
}

func (inst *instance) Info() ([]byte, error) {
    return exec.Command(inst.pool.cfg.UmlctlBin,
        "pool", "status", "--name", inst.pool.cfg.Pool, "--json").Output()
}

func (inst *instance) Close() error {
    if inst.closed { return nil }
    inst.closed = true
    return exec.Command(inst.pool.cfg.UmlctlBin,
        "pool", "destroy", "--name", inst.pool.cfg.Pool,
        "--pid", fmt.Sprint(inst.pid)).Run()
}
```

Line count: ~150 SLoC excluding imports/comments.  The helpers
`allocIPv4` and `allocMAC` (~10 LoC together) bring it to ~160.
Within budget.

---

## 6.  Open questions for the implementation session

  1. **Hostfs scope.**  Does the pool member's hostfs root view the
     same `/` as the host (current umlctl default), or a per-pool
     subtree?  If the latter, `Copy` must `cp` the host file into
     that subtree first — still O(1) verbs, but no longer free.
     CHECK: `umlctl pool serve --rootfs …` default in
     `pool_serve.rs::ServeArgs`.
  2. **mconsole availability.**  The fork-server identity blob
     includes `mconsole_path`, but `pool spawn`'s default is empty.
     For `umlctl exec` to work at all, the daemon must always
     allocate a mconsole socket per-member.  Decision: make
     mconsole MANDATORY (synthesize a path under
     `$XDG_RUNTIME_DIR/uml/pools/<pool>/<instance>.mconsole` when
     unset) before Phase 4 starts.
  3. **PTY vs pipe for `exec` transport.**  §3.1.3 argues PTY.
     Worth a 30-minute prototype with a plain pipe to confirm
     syzkaller's manager does indeed care about line flushing — if
     not, we save ~30 LoC of Rust.
  4. **Concurrency.**  The daemon today single-threads `take`
     (correctly — the identity-memfd protocol is not pipelinable).
     `exec` does NOT need that lock; we can run K execs in
     parallel.  Confirm `pool_serve.rs`'s lock scope before adding
     the `exec` RPC.
  5. **TAP-only forwarding vs DNAT.**  In hosts where the operator
     drops gateway-IP traffic from the guest at the FORWARD chain,
     §4's "TAP-direct" plan fails.  Need a smoke test on a stock
     Debian+ufw host before declaring `umlctl port-forward`
     entirely optional.
  6. **Crash detection ownership.**  syzkaller's report.Reporter
     scans the OutputMerger stream for kernel-oops markers.  As
     long as we emit `{"type":"console", …}` frames during `exec`,
     this Just Works.  If we instead point syzkaller at an external
     console file, we have to wire `report.Reporter` to that file
     too.  Decision: piggy-back on `exec`.
  7. **`Env.SSHKey`/`Env.SSHUser`.**  Unused, but syzkaller's
     manager may probe them on startup.  Stub the config so they're
     accepted-but-ignored; emit a warning if non-empty.
  8. **Snapshot mode.**  `Env.Snapshot=true` is gvisor-style
     pristine-VM-per-program.  Our pool ALREADY gives every program
     a fresh kernel via the fork-server.  Decision: accept and
     ignore `Snapshot`; document that the UML backend is
     intrinsically snapshot-mode.
  9. **Where the backend lives.**  Two options:
     (a) PR into upstream syzkaller, accept the upstream review
         latency;
     (b) maintain a `google/syzkaller` fork rebased weekly.
     §7 covers (b).  Default plan: try (a) first, fall back to (b)
     if a release window slips.
 10. **Versioning.**  `umlctl --version` must be machine-parsable
     so the shim's `Info` returns a stable string.  Today `umlctl
     --version` returns the Cargo `version` field; confirm it
     includes git SHA (PLAN-2026-05-14.md item N).

---

## 7.  Risk: what if syzkaller upstream rejects the backend?

The shim is a vendor-style integration with a non-standard tool
(`umlctl`) outside the syzkaller tree.  Upstream maintainers may
push back on adding a new backend whose external dependency is
not packaged in any distro.  Three scenarios:

  * **Upstream accepts.**  Ideal.  We carry maintenance burden for
    `vm/uml/uml.go` in upstream syzkaller; that's ~150 LoC plus an
    occasional rebase when `vmimpl.Pool` / `vmimpl.Instance`
    change.  Burden is bounded; the interface has been stable since
    2017.

  * **Upstream requests changes.**  Most likely: they ask for the
    backend to depend on a Go-native fork-server library rather
    than shelling to `umlctl`.  Counter-proposal: factor a tiny Go
    client for the pool-daemon JSON-RPC (which already exists), and
    keep `umlctl exec` as the only shell-out (because syzkaller
    DOES shell out to `qemu` already — precedent is on our side).
    Worst case we land the shim with a smaller surface, behind a
    `// build tag uml` build tag they're happy to accept.

  * **Upstream rejects.**  Operator runs a fork.  We carry a public
    fork of `google/syzkaller` at `tools/uml/syzkaller-fork/`
    (or similar), with a single patch: this file.  Cost:
      - one weekly rebase against master;
      - one CI job that proves the fork still builds;
      - one operator instruction page that says
        `go install github.com/UMLPROJECT/syzkaller-uml/syz-manager`
        instead of `…/google/syzkaller/syz-manager`.

    The fork option is cheap because we add ONE FILE.  Diff churn
    against upstream is essentially the (rare) `vmimpl.Pool` /
    `vmimpl.Instance` interface bumps, which we'd have to follow
    anyway as in-tree authors.  This is materially less work than
    maintaining e.g. a kernel out-of-tree driver.

    Decision: target upstream acceptance, but staff the fork as
    Plan B from day one — i.e. write `vm/uml/uml.go` against the
    upstream `vmimpl` interface (which is what we'd do anyway),
    and have an operator README ready that points at the fork.
    The shim spec in §5 is identical in either world.

---

## 8.  Implementation checklist

For the implementation session, in order:

  1. [ ] Land `umlctl pool take` client verb that wraps the
         daemon RPC (~30 LoC Rust + tests).
  2. [ ] Land `umlctl pool status --name <pool> --json` client
         verb (~20 LoC Rust + tests).
  3. [ ] Extend `umlctl pool destroy` with `--name <pool>` to
         route through the daemon socket; keep file-based path
         as default (~20 LoC Rust).
  4. [ ] Make mconsole mandatory in `pool_serve.rs` (synthesize a
         path when caller leaves it empty); document in pool
         serve doc comment.
  5. [ ] Add `{"op":"exec", …}` RPC to the pool daemon: pty
         allocation, mconsole-exec into the member, SCM_RIGHTS
         the pty master back to the client (~120 LoC Rust + a
         smoke test).
  6. [ ] Land `umlctl exec` verb that opens the pty and
         splices stdin/stdout/stderr, with `--json` framing per
         §3.4 (~80 LoC Rust + an integration test that execs
         `/bin/true` and `/bin/false`).
  7. [ ] (Optional, low priority) Land `umlctl port-forward`
         per §3.2.  Defer until a caller needs it.
  8. [ ] Write `vm/uml/uml.go` per §5; build it against a
         vendored copy of the syzkaller `vmimpl` interface to
         confirm it compiles before submitting upstream.
  9. [ ] Smoke test: syz-manager + 4 UML pool members + a tiny
         programs.txt; expect at least one crash inducible by
         a hand-crafted bad syscall (i.e. confirm the loop is
         alive end-to-end, not that fuzzing finds bugs).
 10. [ ] File the upstream PR; in parallel, set up the fork
         build (`Plan B` per §7).

End of memo.
