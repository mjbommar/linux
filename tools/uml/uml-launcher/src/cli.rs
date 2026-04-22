// SPDX-License-Identifier: GPL-2.0
//
// CLI surface. clap-derive.
//
// Subcommand skeleton intentionally leaves room for v2 commands
// (`snapshot`, `attach`, `list`, `stop`). v1 ships `run` + the
// always-there `version`.

use std::path::PathBuf;

use clap::{ArgAction, Parser, Subcommand, ValueEnum};
use serde::{Deserialize, Serialize};

/// Host-side launcher for User-Mode Linux kernels.
///
/// Replaces the hand-rolled shell wrappers that people write to
/// invoke UML. See `uml-launcher run --help` for the workhorse
/// subcommand.
#[derive(Parser, Debug)]
#[command(
    name = "uml-launcher",
    version,
    about = "Host-side launcher for User-Mode Linux kernels",
    long_about = None,
)]
pub struct Cli {
    /// Increase verbosity (-v info, -vv debug, -vvv trace).
    /// Overridden by the RUST_LOG environment variable when set.
    #[arg(short, long, action = ArgAction::Count, global = true)]
    pub verbose: u8,

    /// Log output format.
    #[arg(long, value_enum, default_value_t = LogFormat::Plain, global = true)]
    pub log_format: LogFormat,

    #[command(subcommand)]
    pub command: Command,
}

#[derive(Subcommand, Debug)]
pub enum Command {
    /// Launch a UML kernel.
    Run(RunArgs),

    /// Run a per-device vhost-user backend process (C-10 v2).
    ///
    /// `uml-launcher backend <class>` is the multi-call-binary dispatch
    /// that lets one binary ship all device classes (console, net,
    /// block, …) rather than per-class binaries. Each class is a
    /// separate subcommand so per-class flags stay close to the code
    /// that uses them. Matches the `crosvm device <kind>` shape.
    ///
    /// Currently scaffolding only: every class returns 0 after
    /// emitting a "not yet implemented" line. The real vhost-user
    /// logic, seccomp filters, and LSM transitions land in the
    /// per-class commits that follow (see decisions-log D52 for the
    /// bisectable plan).
    #[command(subcommand)]
    Backend(BackendClass),

    /// Print the launcher version and exit.
    Version,
}

/// Device class served by `uml-launcher backend <class>`.
///
/// The class set mirrors crosvm's device decomposition (minus the
/// niche ones — gpu, snd, wl, pmem — which are v3+ per D52). Each
/// variant carries its own args struct so per-class flags stay
/// type-checked without leaking into the common surface.
#[derive(Subcommand, Debug, Clone)]
pub enum BackendClass {
    /// virtio-console backend. Simplest vhost-user surface: one RX
    /// + one TX queue, byte-oriented.
    Console(BackendConsoleArgs),

    /// virtio-net backend. Tap-backed in v2; slirp is v3+.
    Net(BackendNetArgs),

    /// virtio-blk backend. File-backed image via O_DIRECT.
    Block(BackendBlockArgs),
}

/// Common arguments shared by every `backend <class>` invocation.
///
/// Lives inline in each class's args struct (via `#[command(flatten)]`)
/// rather than as a wrapping struct, so clap's help output shows the
/// common flags under the class they apply to rather than in a
/// surprising "global" position.
#[derive(Parser, Debug, Clone)]
pub struct BackendCommonArgs {
    /// Path to the vhost-user Unix-domain socket this backend will
    /// serve on. UML connects to this socket via
    /// `virtio_uml.device=<socket>:<virtio_id>`.
    #[arg(long, env = "UML_BACKEND_SOCKET")]
    pub socket: PathBuf,
}

/// Per-class args for `backend console`.
#[derive(Parser, Debug, Clone)]
pub struct BackendConsoleArgs {
    #[command(flatten)]
    pub common: BackendCommonArgs,
}

/// Per-class args for `backend net`.
#[derive(Parser, Debug, Clone)]
pub struct BackendNetArgs {
    #[command(flatten)]
    pub common: BackendCommonArgs,

    /// Name of the host tap device to attach.
    #[arg(long, env = "UML_BACKEND_NET_TAP")]
    pub tap: Option<String>,
}

/// Per-class args for `backend block`.
#[derive(Parser, Debug, Clone)]
pub struct BackendBlockArgs {
    #[command(flatten)]
    pub common: BackendCommonArgs,

    /// Path to the disk image file the backend will serve.
    #[arg(long, env = "UML_BACKEND_BLOCK_IMAGE")]
    pub image: Option<PathBuf>,

    /// Expose the image read-only.
    #[arg(long)]
    pub read_only: bool,
}

#[derive(ValueEnum, Clone, Copy, Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum LogFormat {
    #[default]
    Plain,
    Json,
}

#[derive(ValueEnum, Clone, Copy, Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Console {
    /// Wire UML's console to the launcher's stdin/stdout (interactive).
    #[default]
    Stdio,
    /// Drop UML's console; useful for background / headless init.
    Null,
}

/// Arguments to the `run` subcommand.
#[derive(Parser, Debug, Clone)]
pub struct RunArgs {
    /// Path to the UML kernel binary (the `./linux` produced by
    /// `make ARCH=um`).
    #[arg(long, env = "UML_KERNEL")]
    pub kernel: Option<PathBuf>,

    /// Init program inside the guest. Default: /bin/sh.
    #[arg(long, env = "UML_INIT")]
    pub init: Option<PathBuf>,

    /// Guest memory size (accepts `mem=` syntax: 128M, 1G, ...).
    #[arg(long, env = "UML_MEM")]
    pub mem: Option<String>,

    /// Root filesystem mode: `hostfs` (default) or a path to a
    /// ubd image (not yet wired in v1 — hostfs only).
    ///
    /// Left as Option<_> with no clap default so TOML/env layers
    /// can override; the final default lives in Config::default().
    #[arg(long, env = "UML_ROOT")]
    pub root: Option<String>,

    /// Console wiring for the UML kernel.
    ///
    /// Left as Option<_> with no clap default so TOML/env layers
    /// can override; the final default lives in Config::default().
    #[arg(long, value_enum)]
    pub console: Option<Console>,

    /// Plumb host fds 198 (ctl) and 199 (status) into the UML
    /// child for the C-09 AFL forkserver protocol. The CTL fd is
    /// the one the launcher reads from externally (fuzzer →
    /// kernel); STATUS is the one the launcher writes to
    /// (kernel → fuzzer). Accepts `--forkserver=ctl_fd,status_fd`.
    #[arg(long, value_parser = parse_forkserver_fds)]
    pub forkserver: Option<(i32, i32)>,

    /// Extra raw kernel command-line arguments (appended after
    /// the ones the launcher synthesizes). Repeatable.
    #[arg(long = "append")]
    pub append: Vec<String>,

    /// TOML config file to merge with CLI and env vars.
    /// Precedence: CLI > env > file > defaults.
    #[arg(long, env = "UML_CONFIG")]
    pub config: Option<PathBuf>,

    /// Print the synthesized argv + env and exit without
    /// spawning. Useful for debugging the argv builder.
    #[arg(long)]
    pub dry_run: bool,
}

fn parse_forkserver_fds(s: &str) -> Result<(i32, i32), String> {
    let (ctl, status) = s
        .split_once(',')
        .ok_or_else(|| format!("expected `ctl_fd,status_fd`, got {s:?}"))?;
    let ctl: i32 = ctl
        .parse()
        .map_err(|e| format!("bad ctl fd {ctl:?}: {e}"))?;
    let status: i32 = status
        .parse()
        .map_err(|e| format!("bad status fd {status:?}: {e}"))?;
    if ctl < 0 || status < 0 {
        return Err(format!("fds must be non-negative, got ({ctl}, {status})"));
    }
    Ok((ctl, status))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_forkserver_fds_ok() {
        assert_eq!(parse_forkserver_fds("3,4").unwrap(), (3, 4));
        assert_eq!(parse_forkserver_fds("198,199").unwrap(), (198, 199));
    }

    #[test]
    fn parse_forkserver_fds_err() {
        assert!(parse_forkserver_fds("3").is_err());
        assert!(parse_forkserver_fds("3,").is_err());
        assert!(parse_forkserver_fds("-1,4").is_err());
        assert!(parse_forkserver_fds("abc,def").is_err());
    }
}
