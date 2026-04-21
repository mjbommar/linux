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

    /// Print the launcher version and exit.
    Version,
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
    #[arg(long, env = "UML_ROOT", default_value = "hostfs")]
    pub root: String,

    /// Console wiring for the UML kernel.
    #[arg(long, value_enum, default_value_t = Console::default())]
    pub console: Console,

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
