// SPDX-License-Identifier: GPL-2.0
//
// umlctl - multi-instance lifecycle CLI for User-Mode Linux
// instances.
//
// The standard lifecycle verbs are rootless, process-tree-based, and
// filesystem-backed.  State lives in XDG state and runtime dirs so
// independent invocations can coordinate without a global service.
// Pool mode can also run a fork-server daemon for low-latency member
// creation.
//
// Core verbs: create, start, stop, rm, ps, logs, gate, mission,
// pool, exec, and port-forward.
//
// Primary motivation: eliminate the runaway-UML-process class of
// bug — raw `timeout ./linux …` invocations leave the UML child
// alive because UML kernels have their own signal plumbing.
// `umlctl start`/`stop` with a pidfile + blocking waitpid +
// WNOHANG zombie-reap drain closes that gap.

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use mission::MissionArgs;

#[derive(clap::Args, Debug)]
struct StraceArgs {
    /// Instance name.
    name: String,
    /// Extra args passed through to strace (e.g. `-e trace=openat`).
    /// When empty, a reasonable default filter is applied.
    #[arg(last = true)]
    extra: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct GdbArgs {
    /// Instance name.
    name: String,
    /// Extra args passed through to gdb (e.g. `-ex 'b sys_open'`).
    #[arg(last = true)]
    extra: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct BpfArgs {
    /// Instance name.
    name: String,
    /// Pre-canned script name.  Omit (or pass "menu") to list.
    script: Option<String>,
}

#[derive(Subcommand, Debug)]
enum PoolCmd {
    /// Boot a UML in template-pause mode, write its identity blob,
    /// SIGCONT it, and return the host pid (+ instance info as JSON
    /// when --json). This is the unit primitive of the umlctl pool
    /// integration; callers that need a long-lived fork server should
    /// use `pool serve` plus the Unix-socket `take` RPC.
    Spawn(pool::SpawnArgs),
    /// List live pool members previously created by `pool spawn`.
    /// Reads $RUNTIME_DIR/pools/members/*.json + filters out
    /// records whose pid is no longer alive.
    List(pool::ListArgs),
    /// Kill a pool member + remove its record.  Default is SIGKILL;
    /// pass --graceful for SIGTERM + grace period + SIGKILL escalation.
    /// With `--name <pool>`, routes through the daemon socket.
    Destroy(pool::DestroyArgs),
    /// Long-lived supervisor: boots one replicated pool-member master,
    /// accepts take/list/status/destroy/exec/shutdown RPCs on a Unix socket
    /// under $XDG_RUNTIME_DIR/uml/pools/<name>/api.sock.
    Serve(pool_serve::ServeArgs),
    /// Client-side `take`: sends a take RPC to a running `pool serve`
    /// daemon and prints the `SpawnResult` it returns.  Equivalent to
    /// driving the socket protocol from a shell; the syzkaller Go
    /// shim shells out to this verb on every `Create()`.
    Take(pool_take_status::TakeArgs),
    /// Client-side `status`: query the running `pool serve` daemon
    /// for its master pid, member count, and socket path.  Useful for
    /// scripts; the syzkaller shim's `Info()` calls it.
    Status(pool_take_status::StatusArgs),
}
use std::io::{self, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

mod cgroup;
mod console_split;
mod deploy;
mod dmesg_parse;
mod events;
mod exec;
mod gate;
mod gate_loop;
mod history;
mod manifest;
mod mconsole_client;
mod metrics;
mod mission;
mod paths;
mod pool;
mod pool_client;
mod pool_serve;
mod pool_take_status;
mod port_forward;
mod preflight;
mod registry;
mod run;
mod schema;
mod snapshot;
mod supervise;
mod tapfd;
mod transparency;

/// Top-level `umlctl` invocation.
#[derive(Parser, Debug)]
#[command(
    name = "umlctl",
    version,
    about = "Multi-instance lifecycle CLI for User-Mode Linux",
    long_about = "Manage UML instances from the shell. Create registers a \
manifest, start spawns the kernel, ps lists running instances, \
stop/rm clean up. State lives under $XDG_STATE_HOME/uml/ and \
$XDG_RUNTIME_DIR/uml/."
)]
struct Cli {
    /// Override the persistent state directory (default:
    /// $XDG_STATE_HOME/uml or ~/.local/state/uml).
    #[arg(long, global = true, value_name = "PATH")]
    state_dir: Option<std::path::PathBuf>,

    /// Override the runtime (tmpfs) directory (default:
    /// $XDG_RUNTIME_DIR/uml or /tmp/uml-$UID).
    #[arg(long, global = true, value_name = "PATH")]
    runtime_dir: Option<std::path::PathBuf>,

    /// Suppress non-error output.
    #[arg(short, long, global = true)]
    quiet: bool,

    /// Emit machine-readable output (NDJSON for list verbs,
    /// a single JSON object for scalar verbs).
    #[arg(long, global = true)]
    json: bool,

    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Register a new instance manifest. Does NOT spawn.
    Create(CreateArgs),
    /// Spawn the UML process from a manifest.
    Start(StartArgs),
    /// Signal + wait + reap. This is the verb that prevents
    /// runaway UML processes.
    Stop(StopArgs),
    /// Delete manifest + logs.
    Rm(RmArgs),
    /// List instances.
    Ps(PsArgs),
    /// Tail the per-instance log.
    Logs(LogsArgs),
    /// Show only the kernel-printk portion of the run console.
    /// If the bundle has a derived `kernel.log` (written at
    /// stop), prefers that; otherwise filters `init.log`
    /// on the fly.
    Dmesg(DmesgArgs),
    /// One-shot host-side scrape of `/proc/<pid>/*` +
    /// cgroup v2 for the named running instance. O2 lift
    /// of the observability spine.
    Metrics(MetricsArgs),
    /// List declared observability-spine event schemas.
    Schema(SchemaArgs),
    /// Tail structured events.jsonl emitted by the spine.
    Events(EventsArgs),
    /// CI-style predicate check against events.jsonl. Exits 1
    /// on any violation, 0 otherwise. Replaces dmesg-grep in
    /// kselftests.
    Assert(AssertArgs),
    /// Archive a run bundle as a .umlbundle.tar.zst for
    /// sharing / post-mortem. Self-contained: includes
    /// run.json, init.log, events.jsonl, and a snapshot of
    /// the manifest.
    Export(ExportArgs),
    /// Bring up a declarative deployment from an Umlfile (TOML).
    /// Compiles the Umlfile to (host-side TAP+NAT setup, generated
    /// init script, kernel cmdline, manifest), then create+start.
    /// Inspired by docker-compose up.
    Up(UpArgs),
    /// Tear down a deployment: stop the instance, undo host-side
    /// TAP/iptables setup, optionally remove the manifest.
    Down(DownArgs),
    /// Sealed-wrapper test gate: run a Gatefile against an existing
    /// harness, parse PASS/FAIL/EXPECTED_FAIL counts from stdout,
    /// append a scoreboard.jsonl row. The gate runner does NOT
    /// define passing or failing — it reports what the harness
    /// said.
    #[command(subcommand)]
    Gate(GateCmd),
    /// Mission-accomplished gate for the kvm-v2 backend.
    /// Runs the full multi-stage comprehensive check in ~10-15 min:
    /// (1) KUnit, (2) bench, (3) substrate, (4) host_resources,
    /// (5) diverse soak, (6) diagnostic snapshot. Single binary
    /// verdict: MISSION_ACCOMPLISHED / MISSION_FAILED.
    Mission(MissionArgs),
    /// Attach strace to a running UML guest — the guest IS a host
    /// process under both seccomp and kvm-v2 backends, so the host
    /// strace shows every guest syscall.  No other VMM offers this
    /// transparency.
    Strace(StraceArgs),
    /// Attach gdb to a running UML guest with the guest vmlinux
    /// loaded.  Set breakpoints in guest code, inspect guest state,
    /// detach without stopping the guest.  Again, possible only
    /// because the guest is a host process.
    Gdb(GdbArgs),
    /// Run a pre-canned bpftrace script targeting the running UML
    /// guest's PID.  `umlctl bpf <instance>` with no script prints
    /// the menu of available scripts (syscalls / pagefaults / io /
    /// net / sched).
    Bpf(BpfArgs),
    /// Fork-server pool integration.
    #[command(subcommand)]
    Pool(PoolCmd),
    /// Run a command inside a running pool member via the `pool serve`
    /// daemon's exec RPC.  Returns the in-guest command's stdout,
    /// stderr, and exit code.  With `--json`, emits NDJSON frames
    /// shaped for the syzkaller shim.
    Exec(exec::ExecArgs),
    /// Return a host:port address the guest can dial to reach a
    /// host-side service.  TAP-direct mode by default (the guest's
    /// gateway IP is the host).
    #[command(name = "port-forward")]
    PortForward(port_forward::PortForwardArgs),
    /// Snapshot of a running UML guest's KVM-v2 state.
    ///
    /// Today: `export` asks the kernel mconsole `snapshot_export`
    /// command to write an ELF64-core dump to a host path the user provides.
    #[command(subcommand)]
    Snapshot(SnapshotCmd),
}

#[derive(Subcommand, Debug)]
enum SnapshotCmd {
    /// Capture + write an ELF64-core file for a running UML guest.
    /// The file is a gdb / readelf / crash(8) loadable artifact; the
    /// on-disk format is documented at
    /// Documentation/virt/uml/snapshot-elf-format.rst.
    Export(snapshot::ExportArgs),
}

#[derive(Subcommand, Debug)]
enum GateCmd {
    /// Run a Gatefile and append one row to the scoreboard.
    Run(GateRunArgs),
    /// Show recent scoreboard rows as a table with PASS-delta
    /// markers. Use this every commit to spot silent regressions.
    Diff(GateDiffArgs),
    /// List discovered Gatefiles under tools/testing/selftests/um/gates/.
    List(GateListArgs),
    /// Run an Umlfile in a parallel up/wait/classify/stop/rm loop —
    /// the repeated-boot flake-characterization pattern, automated.
    /// Reports PASS/N + Wilson 95% CI.
    /// Optional --sweep KEY=v1,v2,... for env-var sweeps.
    Loop(GateLoopArgs),
}

#[derive(clap::Args, Debug)]
struct GateRunArgs {
    /// Path to the Gatefile (TOML).
    #[arg(short = 'f', long = "file", value_name = "PATH")]
    file: std::path::PathBuf,

    /// UML kernel binary path. Substituted into Gatefile env values
    /// as `{{kernel}}`. Falls back to $UML_KERNEL.
    #[arg(long, value_name = "PATH")]
    kernel: Option<std::path::PathBuf>,

    /// Backend label. Substituted as `{{backend}}`. Default
    /// "seccomp".
    #[arg(long, default_value = "seccomp", value_name = "NAME")]
    backend: String,

    /// Override the scoreboard path. Default
    /// tools/testing/selftests/um/scoreboard.jsonl under the source
    /// root.
    #[arg(long, value_name = "PATH")]
    scoreboard: Option<std::path::PathBuf>,

    /// Print the parsed Gatefile + resolved env without running.
    #[arg(long)]
    dry_run: bool,

    /// Source root (where git rev-parse runs and the default
    /// scoreboard lives). Defaults to the current working dir.
    #[arg(long, value_name = "PATH")]
    source_root: Option<std::path::PathBuf>,
}

#[derive(clap::Args, Debug)]
struct GateDiffArgs {
    /// Filter to a single gate name.
    #[arg(long, value_name = "NAME")]
    gate: Option<String>,

    /// How many recent rows to show.
    #[arg(short = 'n', long, default_value_t = 20)]
    limit: usize,

    /// Override the scoreboard path.
    #[arg(long, value_name = "PATH")]
    scoreboard: Option<std::path::PathBuf>,

    /// Source root (for default scoreboard discovery).
    #[arg(long, value_name = "PATH")]
    source_root: Option<std::path::PathBuf>,
}

#[derive(clap::Args, Debug)]
struct GateListArgs {
    /// Source root (for default gates dir discovery).
    #[arg(long, value_name = "PATH")]
    source_root: Option<std::path::PathBuf>,
}

#[derive(clap::Args, Debug)]
struct GateLoopArgs {
    /// Path to the Umlfile (TOML). The instance.name in the file
    /// is treated as a stem; per-worker copies get suffixes like
    /// `<stem>-w0`, `<stem>-w1`, ...
    #[arg(
        short = 'f',
        long = "file",
        default_value = "Umlfile.toml",
        value_name = "PATH"
    )]
    file: std::path::PathBuf,

    /// Number of parallel workers. Each worker has its own
    /// instance name and runs `iters` boot/test/teardown cycles
    /// sequentially. Cumulative N = workers * iters.
    #[arg(short = 'W', long = "workers", default_value_t = 1, value_name = "W")]
    workers: u32,

    /// Iterations per worker.
    #[arg(short = 'M', long = "iters", default_value_t = 10, value_name = "M")]
    iters: u32,

    /// POSIX ERE (passed to `grep -E`). A line matching this in the
    /// per-iteration init.log marks the iteration as PASS — UNLESS
    /// `--fail-marker` matches first/also (fail wins).
    #[arg(
        long = "pass-marker",
        default_value = "REPRO_DONE rc=0",
        value_name = "REGEX"
    )]
    pass_marker: String,

    /// POSIX ERE: any line matching marks the iteration as FAIL,
    /// even if `--pass-marker` also matches. Default catches the
    /// common KVM-v2 failure modes (mt-mini VERIFY_FAIL, kernel BUG,
    /// kernel panic).
    #[arg(
        long = "fail-marker",
        default_value = "VERIFY_FAIL|kernel BUG|Kernel panic",
        value_name = "REGEX"
    )]
    fail_marker: String,

    /// Per-iteration timeout. Each `up` call gets this as its
    /// readiness budget, and each live run gets this long to hit either
    /// marker before being declared a TIMEOUT (counted as FAIL). 120s
    /// default mirrors the canonical §8c loop budget.
    #[arg(long, default_value_t = 120, value_name = "SECONDS")]
    timeout: u64,

    /// Output directory for per-iteration init.log copies and the
    /// summary file. Default: `/tmp/umlctl-loop-<timestamp>`. The
    /// per-iteration logs let you post-mortem failures even after
    /// `umlctl rm` purged the bundle.
    #[arg(long, value_name = "PATH")]
    out: Option<std::path::PathBuf>,

    /// Env-var sweep: KEY=v1,v2,v3. Repeatable. The full loop runs
    /// once per value (cartesian product across multiple --sweep
    /// flags). Each sweep value is injected into every worker's
    /// Umlfile [env] before that loop starts. Useful for
    /// "compare PASS rate at MT_JITTER_NS=0 vs 100 vs 1000".
    #[arg(long = "sweep", value_name = "KEY=v1,v2,...")]
    sweep: Vec<String>,

    /// Emit one JSON object per sweep point (NDJSON) on stdout.
    /// Otherwise prints a human table.
    #[arg(long)]
    json: bool,

    /// UML kernel binary. Falls back to $UML_KERNEL. Set on the
    /// command line so a sweep can compare two kernels via
    /// `--sweep UML_KERNEL=/path/a,/path/b` without rewriting the
    /// Umlfile.
    #[arg(long, value_name = "PATH")]
    kernel: Option<std::path::PathBuf>,

    /// Override `[network].driver` for every generated worker
    /// Umlfile. Use `vector` for vec0 or `vector2` for vec2.0.
    #[arg(long = "network-driver", value_name = "vector|vector2")]
    network_driver: Option<String>,

    /// Override `[network].queues` for every generated worker
    /// Umlfile. Use `auto` to match each worker's runtime.ncpus.
    #[arg(long = "network-queues", value_name = "N|auto")]
    network_queues: Option<deploy::NetworkQueueSpec>,

    /// Override `[network].host_mode` for every generated worker
    /// Umlfile. `auto` picks launcher-owned inherited fd TAP for
    /// vector2, including multiqueue.
    #[arg(long = "network-host-mode", value_name = "auto|fd|inproc")]
    network_host_mode: Option<String>,

    /// Wrap every generated worker in strace and preserve one
    /// strace-<iter>.log next to the copied init.log.
    #[arg(long)]
    strace: bool,

    /// Fail an iteration if the strace log shows forbidden vector2
    /// sandbox host operations. Implies --strace.
    #[arg(long = "audit-vector-sandbox")]
    audit_vector_sandbox: bool,
}

#[derive(clap::Args, Debug)]
struct CreateArgs {
    /// Instance name. Must match [a-z0-9][a-z0-9_.-]{0,63}.
    name: String,

    /// Path to the UML kernel binary.
    #[arg(long, value_name = "PATH")]
    kernel: std::path::PathBuf,

    /// Profile name (informational; real profile is baked
    /// into the kernel at compile time).
    #[arg(long, value_name = "NAME")]
    profile: Option<String>,

    /// Memory size, e.g. "256M" or "2G".
    #[arg(long, default_value = "256M", value_name = "SIZE")]
    mem: String,

    /// Backend selection (auto, ptrace, seccomp, kvm,
    /// force=<kind>).
    #[arg(long, default_value = "auto", value_name = "KIND")]
    backend: String,

    /// Additional raw kernel command-line arguments.
    #[arg(long, default_value = "", value_name = "RAW")]
    cmdline: String,

    /// Root filesystem source (path or `hostfs`).
    #[arg(long, default_value = "hostfs", value_name = "PATH_OR_HOSTFS")]
    root: String,

    /// Enable forkserver plumbing (fd 198/199).
    #[arg(long)]
    forkserver: bool,

    /// SMP CPU count.
    #[arg(long, default_value_t = 1, value_name = "N")]
    ncpus: u32,

    /// Add a key=value label. Repeatable.
    #[arg(long = "label", value_name = "K=V", action = clap::ArgAction::Append)]
    labels: Vec<String>,

    /// Overwrite an existing manifest with the same name.
    #[arg(long)]
    force: bool,
}

#[derive(clap::Args, Debug)]
struct StartArgs {
    name: String,

    /// Fork + return after the kernel is ready (default).
    #[arg(short, long, conflicts_with = "foreground")]
    detach: bool,

    /// Stream the kernel's console to this terminal and
    /// forward signals. Ctrl-C sends SIGTERM to the UML.
    #[arg(long)]
    foreground: bool,

    /// Wait-for-ready budget before giving up.
    #[arg(long, default_value_t = 30, value_name = "SECONDS")]
    ready_timeout: u64,

    /// Skip creating the per-instance log file.
    #[arg(long)]
    no_log: bool,

    /// Wrap UML in strace and write the host syscall trace here.
    #[arg(long, value_name = "PATH")]
    strace_log: Option<std::path::PathBuf>,
}

#[derive(clap::Args, Debug)]
struct StopArgs {
    name: String,

    /// Initial signal to send (TERM, KILL, INT, QUIT).
    #[arg(short, long, default_value = "TERM", value_name = "SIG")]
    signal: String,

    /// Seconds to wait after the initial signal before
    /// escalating to SIGKILL.
    #[arg(short, long, default_value_t = 10, value_name = "SECONDS")]
    timeout: u64,

    /// Send SIGKILL directly.
    #[arg(short = '9', long)]
    force: bool,
}

#[derive(clap::Args, Debug)]
struct RmArgs {
    name: String,

    /// Stop the instance first if running.
    #[arg(short, long)]
    force: bool,

    /// Delete manifest but keep log files.
    #[arg(long)]
    keep_logs: bool,
}

#[derive(clap::Args, Debug)]
struct PsArgs {
    /// Include stopped instances (manifests whose pid is
    /// absent or dead).
    #[arg(short, long)]
    all: bool,

    /// Names only (pipe-friendly).
    #[arg(short, long)]
    quiet: bool,

    /// Filter: state=running, profile=research,
    /// backend=kvm, label=k=v. Repeatable.
    #[arg(short, long = "filter", value_name = "K=V", action = clap::ArgAction::Append)]
    filters: Vec<String>,

    /// Include an RSS column (one /proc/<pid>/statm read
    /// per listed row).
    #[arg(long)]
    size: bool,
}

#[derive(clap::Args, Debug)]
struct LogsArgs {
    name: String,

    /// `tail -f` behavior.
    #[arg(short, long)]
    follow: bool,

    /// Show only the last N lines.
    #[arg(long, default_value_t = 0, value_name = "N")]
    tail: usize,

    /// Refuse to fall back to the latest saved run bundle.
    /// Without this flag, `umlctl logs <name>` after a fresh `up`
    /// can return content from a previous instance with the same name
    /// if the new instance hasn't yet written a `run_id_file`.
    /// That can mislead repeated test loops: greping for a marker may
    /// return matches from the previous iteration's bundle while the
    /// current kernel is still booting or already crashed. Use
    /// --require-current
    /// in scripts; it exits 7 if no live run is bound.
    #[arg(long)]
    require_current: bool,
}

#[derive(clap::Args, Debug)]
struct DmesgArgs {
    /// Instance name to resolve the latest run from, or a
    /// literal 26-char ULID run_id.
    name_or_run_id: String,

    /// Show only the last N kernel-printk lines.
    #[arg(long, default_value_t = 0, value_name = "N")]
    tail: usize,
}

#[derive(clap::Args, Debug)]
struct MetricsArgs {
    /// Instance name. The pid is resolved from the runtime
    /// pidfile, so the instance must be running.
    name: String,
}

#[derive(clap::Args, Debug)]
struct SchemaArgs {}

#[derive(clap::Args, Debug)]
struct EventsArgs {
    /// Instance name to resolve the latest run_id from, or a
    /// literal 26-char ULID run_id to read directly.
    name_or_run_id: String,

    /// Follow new events as they arrive (like `tail -f`).
    #[arg(short, long)]
    follow: bool,

    /// Show only the last N events before following.
    #[arg(long, default_value_t = 0, value_name = "N")]
    tail: usize,

    /// Filter expressions, repeatable. Form: `<field>=<value>`
    /// where `<field>` matches a top-level JSON key verbatim
    /// (e.g. `event.category=sanitizer`, `schema=uml.panic.v1`,
    /// `pid=12345`). Combined with AND.
    #[arg(long = "filter", value_name = "K=V", action = clap::ArgAction::Append)]
    filters: Vec<String>,

    /// Only events with `host_ts_ns` >= now - <duration>.
    /// Accepts `30s`, `10m`, `2h`, `1d`, or a raw RFC3339
    /// wall-clock instant (compared against `@timestamp`).
    #[arg(long, value_name = "DURATION_OR_RFC3339")]
    since: Option<String>,
}

#[derive(clap::Args, Debug)]
pub struct AssertArgs {
    /// Instance name or literal 26-char ULID run_id.
    pub name_or_run_id: String,

    /// Fail if any event with `schema=uml.panic.v1` is present.
    #[arg(long)]
    pub no_panic: bool,
    /// Fail if any `uml.oom.v1` event is present.
    #[arg(long)]
    pub no_oom: bool,
    /// Fail if any `uml.sanitizer.kasan.v1` event is present.
    #[arg(long)]
    pub no_kasan: bool,
    /// Fail if any `uml.sanitizer.kcsan.v1` event is present.
    #[arg(long)]
    pub no_kcsan: bool,
    /// Fail if any `uml.sanitizer.kmsan.v1` event is present.
    #[arg(long)]
    pub no_kmsan: bool,
    /// Fail if any `uml.sanitizer.kfence.v1` event is present.
    #[arg(long)]
    pub no_kfence: bool,
    /// Fail if any `uml.sanitizer.ubsan.v1` event is present.
    #[arg(long)]
    pub no_ubsan: bool,
    /// Fail if any `uml.rcu_stall.v1` event is present.
    #[arg(long)]
    pub no_rcu_stall: bool,
    /// Fail if any `uml.lockdep.v1` event is present.
    #[arg(long)]
    pub no_lockdep: bool,
    /// Fail if any `uml.watchdog_stall.v1` event is present.
    #[arg(long)]
    pub no_watchdog_stall: bool,

    /// Additional deny predicates: fail if the schema appears.
    /// Repeatable (e.g. `--deny uml.custom.my_check.v1`).
    #[arg(long, value_name = "SCHEMA", action = clap::ArgAction::Append)]
    pub deny: Vec<String>,

    /// Require-at-least predicates: fail if the schema does
    /// NOT appear. Repeatable.
    #[arg(long, value_name = "SCHEMA", action = clap::ArgAction::Append)]
    pub require: Vec<String>,
}

#[derive(clap::Args, Debug)]
struct UpArgs {
    /// Path to the Umlfile (TOML). Defaults to `./Umlfile.toml`.
    #[arg(
        short = 'f',
        long = "file",
        default_value = "Umlfile.toml",
        value_name = "PATH"
    )]
    file: std::path::PathBuf,

    /// Don't actually start the kernel — print synthesized cmdline,
    /// host-side setup steps, and the generated init script.
    #[arg(long)]
    dry_run: bool,

    /// Skip host-side TAP / iptables setup. Useful when the host is
    /// already configured (e.g. in CI with a pre-provisioned bridge).
    #[arg(long)]
    skip_network_setup: bool,

    /// Override `[network].driver` without editing the Umlfile. Use
    /// `vector` for vec0 or `vector2` for vec2.0.
    #[arg(long = "network-driver", value_name = "vector|vector2")]
    network_driver: Option<String>,

    /// Override `[network].queues` without editing the Umlfile. Use
    /// `auto` to match runtime.ncpus. Values above 1, and `auto`,
    /// require vector2.
    #[arg(long = "network-queues", value_name = "N|auto")]
    network_queues: Option<deploy::NetworkQueueSpec>,

    /// Override `[network].host_mode` without editing the Umlfile.
    /// `fd` makes umlctl open TAP queue fds and pass them to vector2.
    #[arg(long = "network-host-mode", value_name = "auto|fd|inproc")]
    network_host_mode: Option<String>,

    /// Wrap UML in `strace -f -s 256 -o <log_dir>/strace.log` to
    /// capture every host syscall the launcher makes. Overrides
    /// debug.strace in the Umlfile.
    #[arg(long)]
    strace: bool,

    /// Run UML under `gdbserver :<port>` so a remote gdb can attach.
    /// Overrides debug.gdb in the Umlfile.
    #[arg(long)]
    gdb: bool,

    /// gdbserver listen port (only with --gdb).
    #[arg(long, default_value_t = 0, value_name = "PORT")]
    gdb_port: u16,

    /// Stay in foreground and stream the kernel's console.
    #[arg(long)]
    foreground: bool,

    /// Wait-for-ready budget before giving up.
    #[arg(long, default_value_t = 60, value_name = "SECONDS")]
    ready_timeout: u64,

    /// After spawn, block until a line matching this POSIX ERE
    /// regex appears in the run's init.log. Pairs with --wait-timeout.
    /// Exit 0 on match, 124 on timeout (matches coreutils `timeout`).
    /// Use this when scripting tests so you don't have to poll
    /// `umlctl logs` manually — and to avoid the stale-log pitfall
    /// where reading logs across an `rm`+`up` cycle returns content
    /// from a prior instance.
    #[arg(long, value_name = "REGEX")]
    wait_for: Option<String>,

    /// Max seconds to wait for `--wait-for`. Default 120.
    #[arg(long, default_value_t = 120, value_name = "SECONDS")]
    wait_timeout: u64,
}

#[derive(clap::Args, Debug)]
struct DownArgs {
    /// Path to the Umlfile (TOML). Defaults to `./Umlfile.toml`.
    #[arg(
        short = 'f',
        long = "file",
        default_value = "Umlfile.toml",
        value_name = "PATH"
    )]
    file: std::path::PathBuf,

    /// Also `umlctl rm` the underlying manifest after stop+teardown.
    #[arg(long)]
    rm: bool,

    /// Stop the instance even if it's already gone (idempotent).
    #[arg(long)]
    force: bool,
}

#[derive(clap::Args, Debug)]
pub struct ExportArgs {
    pub name_or_run_id: String,

    /// Destination file. Suggested extension `.umlbundle.tar.zst`.
    #[arg(long, value_name = "PATH")]
    pub bundle: std::path::PathBuf,
}

fn main() {
    if let Err(e) = run() {
        eprintln!("umlctl: {e:#}");
        // The verbs that set a specific exit code set it via
        // std::process::exit() before returning to main. This
        // generic-fail path is code 1.
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();
    let paths = paths::Paths::resolve(cli.state_dir.clone(), cli.runtime_dir.clone())
        .context("resolve state/runtime directories")?;

    match cli.cmd {
        Cmd::Create(args) => cmd_create(&paths, args),
        Cmd::Start(args) => cmd_start(&paths, args, cli.quiet),
        Cmd::Stop(args) => cmd_stop(&paths, args, cli.quiet),
        Cmd::Rm(args) => cmd_rm(&paths, args, cli.quiet),
        Cmd::Ps(args) => cmd_ps(&paths, args, cli.json, cli.quiet),
        Cmd::Logs(args) => cmd_logs(&paths, args),
        Cmd::Dmesg(args) => cmd_dmesg(&paths, args),
        Cmd::Metrics(args) => cmd_metrics(&paths, args, cli.json),
        Cmd::Schema(_) => cmd_schema(cli.json),
        Cmd::Events(args) => cmd_events(&paths, args),
        Cmd::Assert(args) => cmd_assert(&paths, args, cli.quiet),
        Cmd::Export(args) => cmd_export(&paths, args, cli.quiet),
        Cmd::Up(args) => cmd_up(&paths, args, cli.quiet),
        Cmd::Down(args) => cmd_down(&paths, args, cli.quiet),
        Cmd::Gate(sub) => match sub {
            GateCmd::Run(args) => cmd_gate_run(args, cli.quiet),
            GateCmd::Diff(args) => cmd_gate_diff(args),
            GateCmd::List(args) => cmd_gate_list(args),
            GateCmd::Loop(args) => gate_loop::run(&paths, args, cli.quiet),
        },
        Cmd::Mission(args) => mission::run(args),
        Cmd::Strace(args) => transparency::cmd_strace(&paths, &args.name, &args.extra),
        Cmd::Gdb(args) => transparency::cmd_gdb(&paths, &args.name, &args.extra),
        Cmd::Bpf(args) => transparency::cmd_bpf(&paths, &args.name, args.script.as_deref()),
        Cmd::Pool(sub) => match sub {
            PoolCmd::Spawn(args) => pool::cmd_spawn(args, &paths, cli.quiet),
            PoolCmd::List(args) => pool::cmd_list(args, &paths, cli.quiet),
            PoolCmd::Destroy(args) => pool::cmd_destroy(args, &paths, cli.quiet),
            PoolCmd::Serve(args) => pool_serve::cmd_serve(args, &paths, cli.quiet),
            PoolCmd::Take(args) => pool_take_status::cmd_take(args, &paths, cli.quiet),
            PoolCmd::Status(args) => pool_take_status::cmd_status(args, &paths, cli.quiet),
        },
        Cmd::Exec(args) => exec::cmd_exec(args, &paths, cli.quiet),
        Cmd::PortForward(args) => port_forward::cmd_port_forward(args, &paths, cli.quiet),
        Cmd::Snapshot(sub) => match sub {
            SnapshotCmd::Export(args) => snapshot::cmd_export(&paths, args, cli.quiet),
        },
    }
}

// -------------------------------------------------------------------
// gate
// -------------------------------------------------------------------

fn default_source_root(arg: &Option<std::path::PathBuf>) -> std::path::PathBuf {
    arg.clone()
        .unwrap_or_else(|| std::env::current_dir().unwrap_or_default())
}

fn default_scoreboard(
    arg: &Option<std::path::PathBuf>,
    root: &std::path::Path,
) -> std::path::PathBuf {
    arg.clone()
        .unwrap_or_else(|| root.join("tools/testing/selftests/um/scoreboard.jsonl"))
}

fn cmd_gate_run(args: GateRunArgs, quiet: bool) -> Result<()> {
    let gatefile = gate::Gatefile::from_path(&args.file)
        .with_context(|| format!("load Gatefile {}", args.file.display()))?;
    let kernel = args
        .kernel
        .map(|p| p.to_string_lossy().into_owned())
        .or_else(|| std::env::var("UML_KERNEL").ok())
        .unwrap_or_default();
    let source_root = default_source_root(&args.source_root);
    let scoreboard = default_scoreboard(&args.scoreboard, &source_root);

    if args.dry_run {
        println!("== gate ==");
        println!("  name      = {}", gatefile.gate.name);
        println!("  backend   = {}", args.backend);
        println!("  kernel    = {}", kernel);
        println!("  scoreboard= {}", scoreboard.display());
        println!("  cmd       = {}", gatefile.run.cmd);
        println!("  budget_s  = {}", gatefile.run.budget_sec);
        return Ok(());
    }

    if !quiet {
        println!(
            "[umlctl gate] {} backend={} kernel={}",
            gatefile.gate.name, args.backend, kernel
        );
    }
    let row = gate::run(gate::RunInputs {
        gate: &gatefile,
        backend: &args.backend,
        kernel: &kernel,
        source_root: &source_root,
    })?;
    gate::append_scoreboard(&scoreboard, &row)
        .with_context(|| format!("append {}", scoreboard.display()))?;
    if !quiet {
        println!(
            "[umlctl gate] {} backend={} pass={} fail={} expected_fail={} exit={} dur={:.1}s status={}",
            row.gate, row.backend, row.pass, row.fail, row.expected_fail,
            row.exit_code, row.duration_sec,
            if row.thresholds_met { "PASS" } else { "FAIL" },
        );
        for f in &row.failures {
            println!("    └─ {}", f);
        }
    }
    if !row.thresholds_met {
        std::process::exit(1);
    }
    Ok(())
}

fn cmd_gate_diff(args: GateDiffArgs) -> Result<()> {
    let source_root = default_source_root(&args.source_root);
    let path = default_scoreboard(&args.scoreboard, &source_root);
    let rows = gate::read_scoreboard(&path).with_context(|| format!("read {}", path.display()))?;
    let s = gate::render_diff(&rows, args.gate.as_deref(), args.limit);
    print!("{s}");
    Ok(())
}

fn cmd_gate_list(args: GateListArgs) -> Result<()> {
    let source_root = default_source_root(&args.source_root);
    let dir = source_root.join("tools/testing/selftests/um/gates");
    if !dir.is_dir() {
        println!("(no gates dir at {})", dir.display());
        return Ok(());
    }
    let mut entries: Vec<_> = std::fs::read_dir(&dir)?
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().is_some_and(|x| x == "toml"))
        .collect();
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let p = e.path();
        match gate::Gatefile::from_path(&p) {
            Ok(g) => println!(
                "{name:30} {phase:6} {desc}",
                name = g.gate.name,
                phase = if g.gate.phase.is_empty() {
                    "-"
                } else {
                    &g.gate.phase
                },
                desc = g.gate.description,
            ),
            Err(e) => println!("{}: ERROR ({})", p.display(), e),
        }
    }
    Ok(())
}

// -------------------------------------------------------------------
// up / down
// -------------------------------------------------------------------

fn cmd_up(paths: &paths::Paths, args: UpArgs, quiet: bool) -> Result<()> {
    let mut uml = deploy::Umlfile::from_path(&args.file)
        .with_context(|| format!("load Umlfile {}", args.file.display()))?;

    // CLI debug overrides win.
    if args.strace {
        uml.debug.strace = true;
    }
    if args.gdb {
        uml.debug.gdb = true;
    }
    if args.gdb_port != 0 {
        uml.debug.gdb_port = args.gdb_port;
    }
    if let Some(driver) = args.network_driver.as_deref() {
        deploy::set_network_driver(&mut uml, driver)
            .with_context(|| format!("apply --network-driver {driver}"))?;
    }
    if let Some(queues) = args.network_queues {
        deploy::set_network_queue_spec(&mut uml, queues.clone())
            .with_context(|| format!("apply --network-queues {queues}"))?;
    }
    if let Some(host_mode) = args.network_host_mode.as_deref() {
        deploy::set_network_host_mode(&mut uml, host_mode)
            .with_context(|| format!("apply --network-host-mode {host_mode}"))?;
    }

    let compiled = deploy::compile(&uml).context("compile Umlfile")?;

    if args.dry_run {
        println!(
            "== generated init script ({}) ==",
            compiled.init_script.display()
        );
        println!("{}", std::fs::read_to_string(&compiled.init_script)?);
        print_network_plan(&compiled);
        println!("== host setup steps ==");
        for s in &compiled.setup_steps {
            println!("  sudo sh -c {:?}", s);
        }
        println!("== kernel cmdline appends ==");
        for a in &compiled.append {
            println!("  {a}");
        }
        println!("== teardown steps ==");
        for s in &compiled.teardown_steps {
            println!("  sudo sh -c {:?}", s);
        }
        return Ok(());
    }

    if !args.skip_network_setup && !compiled.setup_steps.is_empty() {
        deploy::run_sudo_steps(&compiled.setup_steps, true, quiet)
            .context("host-side TAP/iptables setup")?;
    }

    // Build the inner cmdline + create the manifest, then start.
    let cmdline_parts: Vec<String> = compiled.append.clone();
    if !uml.kernel.append.is_empty() {
        // already in compiled.append from compile()
    }
    let cmdline = cmdline_parts.join(" ");

    // Create / overwrite manifest. We delete first to keep `up` idempotent.
    let manifest_path = paths.manifest_path(&uml.instance.name);
    if manifest_path.exists() {
        let _ = std::fs::remove_file(&manifest_path);
    }
    let mut labels = uml
        .instance
        .labels
        .iter()
        .map(|(k, v)| format!("{k}={v}"))
        .collect::<Vec<_>>();
    if let Some(plan) = &compiled.network_plan {
        labels.extend(deploy::network_plan_labels(plan));
    }

    let mut m = manifest::Manifest::from_create_args(
        &uml.instance.name,
        std::path::Path::new(&uml.kernel.path),
        Some("umlctl-up"),
        &uml.runtime.mem,
        &uml.kernel.backend,
        &cmdline,
        // When the Umlfile's [runtime].root is something other than
        // "hostfs" (e.g. "ubd", emitted by `umlbuild instance`), pass
        // it through to the manifest so supervise::build_kernel_argv's
        // passthrough arm handles it (no rootfstype=hostfs, no
        // root=/dev/root).
        &uml.runtime.root,
        false,
        uml.runtime.ncpus,
        &labels,
    )
    .context("build manifest from Umlfile")?;
    /* Translate Umlfile.host_resources into host_env + cgroup_v2 on
     * the manifest. Empty fields leave the manifest unchanged. */
    deploy::apply_host_resources(&uml.host_resources, &mut m);

    /*
     * When [runtime].fast_boot is true, also export UM_FAST_BOOT=1 so
     * the host-side preflight prints in
     * os_early_checks() (which run BEFORE the kernel cmdline parser
     * fires) are silenced.  See arch/um/os-Linux/util.c::os_info for
     * the env-var consumer.  Saves the per-line stderr write()
     * round-trips when the supervisor's stderr is a pipe (~10 ms
     * cumulative on slow hosts).
     */
    if uml.runtime.fast_boot {
        m.host_env.insert("UM_FAST_BOOT".into(), "1".into());
    }
    std::fs::create_dir_all(manifest_path.parent().unwrap())
        .context("create instances directory")?;
    m.write_to(&manifest_path).context("write manifest")?;

    // For hostfs-rooted instances, map the Umlfile's compiled
    // init.sh into init=PATH on the cmdline.  For ubd-rooted
    // instances (umlbuild output), the rootfs ships its own
    // /sbin/init — don't override.
    let mut full_cmdline = cmdline.clone();
    if uml.runtime.root == "hostfs" {
        let init_arg = format!("init={}", compiled.init_script.display());
        if !full_cmdline.is_empty() {
            full_cmdline.push(' ');
        }
        full_cmdline.push_str(&init_arg);
    }
    // Rewrite manifest with the (possibly init=-augmented) cmdline.
    let mut m2 = m.clone();
    m2.runtime.cmdline = full_cmdline;
    m2.write_to(&manifest_path)?;

    if !quiet {
        eprintln!(
            "[umlctl] up: instance={} init={}",
            uml.instance.name,
            compiled.init_script.display()
        );
        if let Some(plan) = &compiled.network_plan {
            eprintln!(
                "[umlctl] network: driver={} guest_dev={} tap={} transport={} host_mode={} queues={} queue_spec={}",
                plan.driver,
                plan.guest_dev,
                plan.tap_name,
                plan.transport,
                plan.host_mode,
                plan.queue_count,
                plan.queue_spec,
            );
            if let Some(fd) = plan.inherited_fd {
                eprintln!(
                    "[umlctl] network-fd: open tap={} and inherit {}",
                    plan.tap_name,
                    format_inherited_fd_range(fd, plan.inherited_fd_count),
                );
            }
        }
    }
    let start_args = StartArgs {
        name: uml.instance.name.clone(),
        detach: !args.foreground,
        foreground: args.foreground,
        ready_timeout: args.ready_timeout,
        no_log: false,
        strace_log: if uml.debug.strace {
            Some(
                std::env::current_dir()
                    .context("getcwd")?
                    .join(&uml.debug.log_dir)
                    .join("strace.log"),
            )
        } else {
            None
        },
    };
    cmd_start(paths, start_args, quiet)?;

    if let Some(pat) = &args.wait_for {
        // Block until the spawn's init.log has a line matching `pat`.
        // Resolves the stale-log race: the run_id_file is written by
        // cmd_start above, so we can pick out THIS instance's bundle
        // unambiguously rather than racing `umlctl logs <name>` which
        // can return content from a previous rm'd-and-respawned
        // instance with the same name. Exits 124 on timeout.
        wait_for_marker(paths, &uml.instance.name, pat, args.wait_timeout, quiet);
    }
    Ok(())
}

fn print_network_plan(compiled: &deploy::Compiled) {
    println!("== network plan ==");
    if let Some(plan) = &compiled.network_plan {
        println!(
            "  mode=tap driver={} guest_dev={} host_tap={} transport={} host_mode={} queues={} queue_spec={}",
            plan.driver,
            plan.guest_dev,
            plan.tap_name,
            plan.transport,
            plan.host_mode,
            plan.queue_count,
            plan.queue_spec,
        );
        println!("  kernel_arg={}", plan.kernel_arg);
        if let Some(fd) = plan.inherited_fd {
            println!(
                "  inherited_fds=tap:{} -> {}",
                plan.tap_name,
                format_inherited_fd_range(fd, plan.inherited_fd_count)
            );
        }
    } else {
        println!("  mode=none");
    }
}

fn format_inherited_fd_range(first_fd: i32, count: u32) -> String {
    if count <= 1 {
        format!("fd={first_fd}")
    } else {
        let last_fd = first_fd + count as i32 - 1;
        format!("fds={first_fd}..{last_fd}")
    }
}

fn fd_in_inherited_range(fd: i32, first_fd: i32, count: i32) -> bool {
    count > 0 && fd >= first_fd && fd < first_fd + count
}

fn move_fd_out_of_inherited_range(fd: OwnedFd, first_fd: i32, count: i32) -> io::Result<OwnedFd> {
    if !fd_in_inherited_range(fd.as_raw_fd(), first_fd, count) {
        return Ok(fd);
    }

    let min_fd = first_fd
        .checked_add(count)
        .ok_or_else(|| io::Error::from_raw_os_error(libc::EINVAL))?;
    let new_fd = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_DUPFD_CLOEXEC, min_fd) };
    if new_fd < 0 {
        return Err(io::Error::last_os_error());
    }

    Ok(unsafe { OwnedFd::from_raw_fd(new_fd) })
}

/// Poll the named instance's CURRENT run bundle's init.log for a line
/// matching `pattern` (POSIX ERE — same semantics as `umlctl gate`).
/// Exits the process on timeout (code 124, matches coreutils
/// `timeout`); returns normally on match.
///
/// Intentionally only consults the live `run_id_file` — never falls
/// back to the latest saved bundle. That fallback is the source
/// of the stale-log pitfall this verb is designed to avoid.
fn wait_for_marker(
    paths: &paths::Paths,
    name: &str,
    pattern: &str,
    timeout_secs: u64,
    quiet: bool,
) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(timeout_secs);
    let run_id_path = paths.run_id_file_path(name);
    loop {
        if let Some(run_id) = supervise::read_run_id_file(&run_id_path) {
            let init_log = paths.run_dir(&run_id).join("init.log");
            if init_log.exists() && grep_file_matches(pattern, &init_log) {
                if !quiet {
                    eprintln!(
                        "[umlctl] wait-for matched /{pattern}/ in {}",
                        init_log.display()
                    );
                }
                return;
            }
        }
        if std::time::Instant::now() >= deadline {
            eprintln!(
                "umlctl: wait-for timeout ({timeout_secs}s) waiting for /{pattern}/ in instance '{name}'"
            );
            std::process::exit(124);
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
}

/// Run `grep -E -q <pattern> <file>`. Returns true on match. Mirrors
/// the existing `count_lines_matching` helper in gate.rs (same regex
/// dialect, no extra Rust dep).
fn grep_file_matches(pattern: &str, file: &std::path::Path) -> bool {
    use std::process::Command;
    let Some(file_s) = file.to_str() else {
        return false;
    };
    Command::new("grep")
        .args(["-E", "-q", pattern, file_s])
        .status()
        .map(|st| st.success())
        .unwrap_or(false)
}

fn cmd_down(paths: &paths::Paths, args: DownArgs, quiet: bool) -> Result<()> {
    let uml = deploy::Umlfile::from_path(&args.file)
        .with_context(|| format!("load Umlfile {}", args.file.display()))?;

    // Stop the instance (best-effort if --force). Call the supervisor
    // directly instead of `cmd_stop`: the command wrapper exits the
    // process on "not running", but `down --force` still needs to run
    // host-side teardown for partially-started TAP instances.
    let stop_args = StopArgs {
        name: uml.instance.name.clone(),
        signal: "TERM".into(),
        timeout: 10,
        force: args.force,
    };
    match supervise::stop(paths, &stop_args) {
        Ok(info) => {
            if !quiet {
                println!(
                    "stopped {} pid={} signal={} exit={:?} run_id={}",
                    uml.instance.name, info.pid, info.signal_sent, info.exit_status, info.run_id
                );
            }
            history::append(
                paths,
                history::Event::Stop {
                    name: &uml.instance.name,
                    pid: info.pid,
                    run_id: &info.run_id,
                    exit_status: info.exit_status,
                    signal_sent: &info.signal_sent,
                },
            )?;
            events::emit_lifecycle(
                paths,
                &info.run_id,
                &uml.instance.name,
                "stop",
                Some(info.pid),
                Some(&info.signal_sent),
                info.exit_status,
            )?;
        }
        Err(supervise::StopError::ManifestMissing | supervise::StopError::NotRunning)
            if args.force =>
        {
            if !quiet {
                eprintln!("[umlctl] (instance not running; --force, continuing)");
            }
        }
        Err(supervise::StopError::ManifestMissing) => {
            bail!("instance '{}' not found", uml.instance.name);
        }
        Err(supervise::StopError::NotRunning) => {
            bail!("instance '{}' not running", uml.instance.name);
        }
        Err(supervise::StopError::Other(e)) => {
            if !args.force {
                return Err(e);
            }
            if !quiet {
                eprintln!("[umlctl] (stop failed: {e:#}; --force, continuing)");
            }
        }
    }

    // Compile to get teardown steps. Failures are non-fatal because
    // setup may have been partial.
    if let Ok(compiled) = deploy::compile(&uml) {
        if !compiled.teardown_steps.is_empty() {
            let _ = deploy::run_sudo_steps(&compiled.teardown_steps, false, quiet);
        }
    }

    if args.rm {
        if paths.manifest_path(&uml.instance.name).exists() {
            let rm_args = RmArgs {
                name: uml.instance.name.clone(),
                force: true,
                keep_logs: false,
            };
            cmd_rm(paths, rm_args, quiet)?;
        } else if !quiet {
            eprintln!("[umlctl] (manifest already absent; --rm, continuing)");
        }
    }
    Ok(())
}

fn cmd_events(paths: &paths::Paths, args: EventsArgs) -> Result<()> {
    events::cmd_tail(paths, &args)
}

fn cmd_assert(paths: &paths::Paths, args: AssertArgs, quiet: bool) -> Result<()> {
    events::cmd_assert(paths, &args, quiet)
}

fn cmd_export(paths: &paths::Paths, args: ExportArgs, quiet: bool) -> Result<()> {
    events::cmd_export(paths, &args, quiet)
}

fn cmd_schema(json: bool) -> Result<()> {
    if json {
        schema::print_json()
    } else {
        schema::print_human();
        Ok(())
    }
}

fn cmd_create(paths: &paths::Paths, args: CreateArgs) -> Result<()> {
    manifest::validate_name(&args.name)?;
    let manifest_path = paths.manifest_path(&args.name);
    if manifest_path.exists() && !args.force {
        eprintln!(
            "umlctl: instance '{}' already exists (use --force to overwrite)",
            args.name
        );
        std::process::exit(4);
    }

    let m = manifest::Manifest::from_create_args(
        &args.name,
        &args.kernel,
        args.profile.as_deref(),
        &args.mem,
        &args.backend,
        &args.cmdline,
        &args.root,
        args.forkserver,
        args.ncpus,
        &args.labels,
    )
    .context("build manifest")?;

    std::fs::create_dir_all(manifest_path.parent().unwrap())
        .context("create instances directory")?;
    m.write_to(&manifest_path).context("write manifest")?;
    history::append(paths, history::Event::Create { name: &args.name })?;

    println!("created {}", args.name);
    Ok(())
}

fn cmd_start(paths: &paths::Paths, args: StartArgs, quiet: bool) -> Result<()> {
    let m = match manifest::Manifest::read(&paths.manifest_path(&args.name)) {
        Ok(m) => m,
        Err(e) => {
            if e.downcast_ref::<std::io::Error>()
                .map(|io| io.kind() == std::io::ErrorKind::NotFound)
                .unwrap_or(false)
            {
                eprintln!("umlctl: instance '{}' not found", args.name);
                std::process::exit(3);
            }
            return Err(e);
        }
    };

    if let Some(ident) = supervise::read_pidfile(&paths.pidfile_path(&args.name)) {
        if supervise::identity_alive(ident) {
            eprintln!(
                "umlctl: instance '{}' already running (pid {})",
                args.name, ident.pid
            );
            std::process::exit(4);
        }
    }
    if !m.kernel.path.exists() {
        eprintln!(
            "umlctl: kernel '{}' missing or not executable",
            m.kernel.path.display()
        );
        std::process::exit(5);
    }

    let inherited_fds = prepare_inherited_fds(&m)?;

    match supervise::start_with_fds(paths, &m, &args, &inherited_fds.mappings) {
        Ok(outcome) => {
            if !quiet {
                println!(
                    "started {} pid={} run_id={}",
                    args.name, outcome.pid, outcome.run_id
                );
            }
            history::append(
                paths,
                history::Event::Start {
                    name: &args.name,
                    pid: outcome.pid,
                    run_id: &outcome.run_id,
                },
            )?;
            events::emit_lifecycle(
                paths,
                &outcome.run_id,
                &args.name,
                "start",
                Some(outcome.pid),
                None,
                None,
            )?;
            Ok(())
        }
        Err(supervise::StartError::AlreadyRunning { pid }) => {
            eprintln!(
                "umlctl: instance '{}' already running (pid {})",
                args.name, pid
            );
            std::process::exit(4);
        }
        Err(supervise::StartError::KernelMissing(p)) => {
            eprintln!("umlctl: kernel '{}' missing or not executable", p.display());
            std::process::exit(5);
        }
        Err(supervise::StartError::ReadyTimeout {
            pid,
            run_id,
            log_path,
        }) => {
            let log = log_path
                .as_ref()
                .map(|p| p.display().to_string())
                .unwrap_or_else(|| "-".to_string());
            eprintln!(
                "umlctl: instance '{}' did not become ready within {}s pid={} run_id={} init_log={}",
                args.name, args.ready_timeout, pid, run_id, log
            );
            std::process::exit(124);
        }
        Err(supervise::StartError::Other(e)) => Err(e),
    }
}

struct PreparedInheritedFds {
    _holders: Vec<OwnedFd>,
    mappings: Vec<supervise::InheritedFd>,
}

fn prepare_inherited_fds(m: &manifest::Manifest) -> Result<PreparedInheritedFds> {
    let mut holders = Vec::new();
    let mut mappings = Vec::new();

    if m.labels
        .get(deploy::LABEL_NETWORK_DRIVER)
        .map(String::as_str)
        == Some("vector2")
        && m.labels
            .get(deploy::LABEL_NETWORK_TRANSPORT)
            .map(String::as_str)
            == Some("fd")
        && m.labels
            .get(deploy::LABEL_NETWORK_HOST_MODE)
            .map(String::as_str)
            == Some("fd")
    {
        let tap = m
            .labels
            .get(deploy::LABEL_NETWORK_TAP_NAME)
            .context("manifest is missing vector2 fd tap label")?;
        let target_fd = m
            .labels
            .get(deploy::LABEL_NETWORK_FD)
            .context("manifest is missing vector2 inherited fd label")?
            .parse::<i32>()
            .context("parse vector2 inherited fd label")?;
        let fd_count = m
            .labels
            .get(deploy::LABEL_NETWORK_FD_COUNT)
            .or_else(|| m.labels.get(deploy::LABEL_NETWORK_QUEUES))
            .map(|s| {
                s.parse::<u32>()
                    .context("parse vector2 inherited fd count label")
            })
            .transpose()?
            .unwrap_or(1);
        if fd_count == 0 {
            bail!("vector2 inherited fd count must be >= 1");
        }
        if fd_count > 1024 {
            bail!("vector2 inherited fd count must be <= 1024");
        }
        if target_fd < 0 {
            bail!("vector2 inherited fd target must be non-negative");
        }
        let fd_count_i32 =
            i32::try_from(fd_count).context("vector2 inherited fd count exceeds i32")?;
        let last_fd = target_fd
            .checked_add(fd_count_i32 - 1)
            .context("vector2 inherited fd range overflows i32")?;
        let multi_queue = fd_count > 1;

        for offset in 0..fd_count {
            let current_target_fd = target_fd + offset as i32;
            let fd = tapfd::open_tap(tap, multi_queue).with_context(|| {
                format!(
                    "open TAP {tap} queue {queue}/{fd_count} for vector2 inherited fd handoff \
                     ({range}); check that `umlctl up` created it for this user",
                    queue = offset + 1,
                    range = format_inherited_fd_range(target_fd, fd_count),
                )
            })?;
            let fd = move_fd_out_of_inherited_range(fd, target_fd, fd_count_i32)
                .context("move vector2 TAP source fd out of inherited target range")?;
            mappings.push(supervise::InheritedFd {
                source_fd: fd.as_raw_fd(),
                target_fd: current_target_fd,
            });
            holders.push(fd);
        }
        debug_assert_eq!(last_fd, target_fd + fd_count_i32 - 1);
    }

    Ok(PreparedInheritedFds {
        _holders: holders,
        mappings,
    })
}

fn cmd_stop(paths: &paths::Paths, args: StopArgs, quiet: bool) -> Result<()> {
    match supervise::stop(paths, &args) {
        Ok(info) => {
            if !quiet {
                println!(
                    "stopped {} pid={} signal={} exit={:?} run_id={}",
                    args.name, info.pid, info.signal_sent, info.exit_status, info.run_id
                );
            }
            history::append(
                paths,
                history::Event::Stop {
                    name: &args.name,
                    pid: info.pid,
                    run_id: &info.run_id,
                    exit_status: info.exit_status,
                    signal_sent: &info.signal_sent,
                },
            )?;
            events::emit_lifecycle(
                paths,
                &info.run_id,
                &args.name,
                "stop",
                Some(info.pid),
                Some(&info.signal_sent),
                info.exit_status,
            )?;
            Ok(())
        }
        Err(supervise::StopError::ManifestMissing) => {
            eprintln!("umlctl: instance '{}' not found", args.name);
            std::process::exit(3);
        }
        Err(supervise::StopError::NotRunning) => {
            eprintln!("umlctl: instance '{}' not running", args.name);
            std::process::exit(6);
        }
        Err(supervise::StopError::Other(e)) => Err(e),
    }
}

fn cmd_rm(paths: &paths::Paths, args: RmArgs, quiet: bool) -> Result<()> {
    let manifest_path = paths.manifest_path(&args.name);
    if !manifest_path.exists() {
        eprintln!("umlctl: instance '{}' not found", args.name);
        std::process::exit(3);
    }

    if supervise::is_running(paths, &args.name) {
        if !args.force {
            eprintln!(
                "umlctl: instance '{}' still running (use --force to stop-then-remove)",
                args.name
            );
            std::process::exit(5);
        }
        let stop_args = StopArgs {
            name: args.name.clone(),
            signal: "TERM".into(),
            timeout: 10,
            force: false,
        };
        cmd_stop(paths, stop_args, /* quiet = */ true)?;
    }

    // Delete manifest.
    std::fs::remove_file(&manifest_path).context("remove manifest")?;

    // Delete per-run bundle directories (unless --keep-logs).
    // Each bundle belongs to exactly one instance per
    // run.json.instance, so we only touch this instance's
    // runs; a shared runs/ root stays intact for other
    // instances.
    if !args.keep_logs {
        for run_id in run::runs_for(paths, &args.name) {
            let dir = paths.run_dir(&run_id);
            if dir.exists() {
                std::fs::remove_dir_all(&dir).ok();
            }
        }
    }

    // Delete any residual runtime files.
    let _ = std::fs::remove_file(paths.pidfile_path(&args.name));
    let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));

    history::append(paths, history::Event::Rm { name: &args.name })?;
    if !quiet {
        println!("removed {}", args.name);
    }
    Ok(())
}

fn cmd_ps(paths: &paths::Paths, args: PsArgs, json: bool, _quiet: bool) -> Result<()> {
    let rows = registry::list(paths, &args)?;
    if json {
        registry::print_json(&rows)
    } else {
        registry::print_table(&rows, &args)
    }
}

fn cmd_logs(paths: &paths::Paths, args: LogsArgs) -> Result<()> {
    let manifest_path = paths.manifest_path(&args.name);
    if !manifest_path.exists() {
        eprintln!("umlctl: instance '{}' not found", args.name);
        std::process::exit(3);
    }
    // Resolve the most recent run bundle and pull init.log
    // from it. Prefer the live run_id side-file (set while
    // the instance is running); fall back to a scan of
    // $STATE/runs/ by creation order — UNLESS --require-current
    // forbids the fallback (avoids the stale-log pitfall in
    // test loops that grep across rm/up cycles).
    let live_run = supervise::read_run_id_file(&paths.run_id_file_path(&args.name));
    let run_id = if args.require_current {
        if live_run.is_none() {
            eprintln!(
                "umlctl: instance '{}' has no live run (--require-current): no run_id_file present",
                args.name
            );
            std::process::exit(7);
        }
        live_run
    } else {
        live_run.or_else(|| run::latest_run_for(paths, &args.name))
    };
    let log = match run_id {
        Some(id) => paths.run_dir(&id).join("init.log"),
        None => {
            eprintln!("umlctl: no log file for instance '{}'", args.name);
            std::process::exit(7);
        }
    };
    if !log.exists() {
        eprintln!("umlctl: no log file for instance '{}'", args.name);
        std::process::exit(7);
    }
    let content = std::fs::read_to_string(&log).context("read log file")?;
    let lines: Vec<&str> = content.lines().collect();
    let start = if args.tail > 0 && args.tail < lines.len() {
        lines.len() - args.tail
    } else {
        0
    };
    for line in &lines[start..] {
        println!("{line}");
    }
    if args.follow {
        // v1: simple tail-f via a blocking read loop on the file.
        // Good enough for interactive use; proper inotify is v2.
        use std::io::{BufRead, BufReader, Seek};
        let mut f = std::fs::File::open(&log).context("reopen log for follow")?;
        f.seek(std::io::SeekFrom::End(0)).ok();
        let mut reader = BufReader::new(f);
        loop {
            let mut line = String::new();
            match reader.read_line(&mut line) {
                Ok(0) => std::thread::sleep(std::time::Duration::from_millis(200)),
                Ok(_) => {
                    print!("{line}");
                }
                Err(e) => return Err(e.into()),
            }
        }
    }
    Ok(())
}

fn cmd_metrics(paths: &paths::Paths, args: MetricsArgs, json: bool) -> Result<()> {
    // Manifest presence gate mirrors logs/dmesg so a typo
    // returns "instance not found" (exit 3) rather than a
    // confusing "no pid."
    let manifest_path = paths.manifest_path(&args.name);
    if !manifest_path.exists() {
        eprintln!("umlctl: instance '{}' not found", args.name);
        std::process::exit(3);
    }
    let pidfile = paths.pidfile_path(&args.name);
    let pid = match supervise::read_pidfile(&pidfile) {
        Some(ident) if supervise::identity_alive(ident) => ident.pid,
        _ => {
            eprintln!("umlctl: instance '{}' not running", args.name);
            std::process::exit(6);
        }
    };

    let m = metrics::scrape(pid).context("scrape /proc")?;
    let stdout = std::io::stdout();
    let mut w = stdout.lock();
    if json {
        metrics::render_json(&m, &mut w)?;
        writeln!(&mut w)?;
    } else {
        metrics::render_human(&m, &mut w)?;
    }
    Ok(())
}

fn cmd_dmesg(paths: &paths::Paths, args: DmesgArgs) -> Result<()> {
    let run_id = events::resolve_name_or_run_id(paths, &args.name_or_run_id);
    let run_dir = paths.run_dir(&run_id);
    let kernel_log = run_dir.join("kernel.log");
    let init_log = run_dir.join("init.log");

    // Prefer the materialized kernel.log if `umlctl stop`
    // already derived it; fall back to an on-the-fly filter
    // over init.log when the run is still live (stop hasn't
    // happened) or for bundles created before kernel.log sidecars.
    let lines: Vec<String> = if kernel_log.exists() {
        std::fs::read_to_string(&kernel_log)
            .context("read kernel.log")?
            .lines()
            .map(String::from)
            .collect()
    } else if init_log.exists() {
        let mut buf = Vec::new();
        console_split::stream_kernel_lines(&init_log, &mut buf)
            .context("filter init.log for kernel lines")?;
        String::from_utf8_lossy(&buf)
            .lines()
            .map(String::from)
            .collect()
    } else {
        eprintln!("umlctl: no console log for run '{run_id}'");
        std::process::exit(7);
    };

    let start = if args.tail > 0 && args.tail < lines.len() {
        lines.len() - args.tail
    } else {
        0
    };
    for line in &lines[start..] {
        println!("{line}");
    }
    Ok(())
}

#[cfg(test)]
mod inherited_fd_tests {
    use super::*;
    use std::fs::File;

    #[test]
    fn inherited_fd_range_checks_half_open_interval() {
        assert!(!fd_in_inherited_range(199, 200, 4));
        assert!(fd_in_inherited_range(200, 200, 4));
        assert!(fd_in_inherited_range(203, 200, 4));
        assert!(!fd_in_inherited_range(204, 200, 4));
        assert!(!fd_in_inherited_range(200, 200, 0));
    }

    #[test]
    fn source_fd_is_moved_out_of_target_range_when_needed() {
        let f = File::open("/dev/null").unwrap();
        let first_fd = 200;
        let count = 4;
        let raw = unsafe { libc::fcntl(f.as_raw_fd(), libc::F_DUPFD_CLOEXEC, first_fd) };
        assert!(raw >= 0);

        let original_in_range = fd_in_inherited_range(raw, first_fd, count);
        let fd = unsafe { OwnedFd::from_raw_fd(raw) };
        let moved = move_fd_out_of_inherited_range(fd, first_fd, count).unwrap();

        assert!(!fd_in_inherited_range(moved.as_raw_fd(), first_fd, count));
        if original_in_range {
            assert!(moved.as_raw_fd() >= first_fd + count);
        }
    }
}
