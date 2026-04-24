// SPDX-License-Identifier: GPL-2.0
//
// umlctl v1 — multi-instance lifecycle CLI for User-Mode Linux
// instances. See Documentation/virt/uml/redesign/08-future-phases/
// 05-umlctl.md for the authoritative spec.
//
// Podman-shaped, not Docker-shaped: rootless, no daemon,
// process-tree-based. Every invocation exits. State lives on
// the filesystem (XDG state + runtime dirs) so there's no need
// to serialize between calls.
//
// v1 verbs: create, start, stop, rm, ps, logs.
// Deferred: top, stat, exec, attach, backend (v2+).
//
// Primary motivation: eliminate the runaway-UML-process class of
// bug — raw `timeout ./linux …` invocations leave the UML child
// alive because UML kernels have their own signal plumbing.
// `umlctl start`/`stop` with a pidfile + blocking waitpid +
// WNOHANG zombie-reap drain closes that gap.

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod events;
mod history;
mod manifest;
mod paths;
mod registry;
mod run;
mod schema;
mod supervise;

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
    /// List declared observability-spine event schemas.
    Schema(SchemaArgs),
    /// Tail structured events.jsonl emitted by the spine.
    Events(EventsArgs),
    /// CI-style predicate check against events.jsonl. Exits 1
    /// on any violation, 0 otherwise. Replaces dmesg-grep in
    /// kselftests.
    Assert(AssertArgs),
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
        Cmd::Schema(_) => cmd_schema(cli.json),
        Cmd::Events(args) => cmd_events(&paths, args),
        Cmd::Assert(args) => cmd_assert(&paths, args, cli.quiet),
    }
}

fn cmd_events(paths: &paths::Paths, args: EventsArgs) -> Result<()> {
    events::cmd_tail(paths, &args)
}

fn cmd_assert(paths: &paths::Paths, args: AssertArgs, quiet: bool) -> Result<()> {
    events::cmd_assert(paths, &args, quiet)
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
    let m = manifest::Manifest::read(&paths.manifest_path(&args.name)).map_err(|e| {
        if e.downcast_ref::<std::io::Error>()
            .map(|io| io.kind() == std::io::ErrorKind::NotFound)
            .unwrap_or(false)
        {
            eprintln!("umlctl: instance '{}' not found", args.name);
            std::process::exit(3);
        }
        e
    })?;

    match supervise::start(paths, &m, &args) {
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
            eprintln!(
                "umlctl: kernel '{}' missing or not executable",
                p.display()
            );
            std::process::exit(5);
        }
        Err(supervise::StartError::ReadyTimeout) => {
            eprintln!(
                "umlctl: instance '{}' did not become ready within {}s",
                args.name, args.ready_timeout
            );
            std::process::exit(124);
        }
        Err(supervise::StartError::Other(e)) => Err(e),
    }
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
    // $STATE/runs/ by creation order.
    let run_id = supervise::read_run_id_file(&paths.run_id_file_path(&args.name))
        .or_else(|| run::latest_run_for(paths, &args.name));
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
