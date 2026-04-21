// SPDX-License-Identifier: GPL-2.0
//
// uml-launcher — host-side launcher for User-Mode Linux kernels.
//
// Workstream C-10 of the UML redesign. See
// Documentation/virt/uml/redesign/02-workstreams/
// C-profiles-and-gaps/10-host-launcher-crosvm.md for the design
// and Documentation/virt/uml/launcher.rst for the user-facing
// reference.
//
// v1 replaces the hand-rolled shell wrappers that people write
// to invoke UML. v2 will add per-device vhost-user processes
// with seccomp filters, matching crosvm/Firecracker posture.

use std::process::ExitCode;

use anyhow::Result;
use clap::Parser;

mod cli;
mod config;
mod launcher;
mod signal;

fn main() -> ExitCode {
    let args = cli::Cli::parse();

    init_tracing(args.verbose, args.log_format);

    match run(args) {
        Ok(code) => {
            // Exit-code passthrough: the launcher's exit code is
            // the UML child's exit code. This lets harnesses key
            // off `$?` the same as they would with a direct
            // invocation.
            tracing::debug!(exit_code = code, "launcher exiting");
            ExitCode::from(code as u8)
        }
        Err(e) => {
            // Print the full anyhow chain + any `.context(...)`
            // layers; RUST_BACKTRACE=1 adds a backtrace.
            tracing::error!("uml-launcher: {:#}", e);
            ExitCode::FAILURE
        }
    }
}

fn run(args: cli::Cli) -> Result<i32> {
    match args.command {
        cli::Command::Run(run_args) => {
            let cfg = config::load(&run_args)?;
            launcher::run(cfg)
        }
        cli::Command::Version => {
            println!("uml-launcher {}", env!("CARGO_PKG_VERSION"));
            Ok(0)
        }
    }
}

fn init_tracing(verbosity: u8, format: cli::LogFormat) {
    use tracing_subscriber::{fmt, EnvFilter};

    // -v → info, -vv → debug, -vvv → trace. RUST_LOG overrides.
    let default_level = match verbosity {
        0 => "warn",
        1 => "info",
        2 => "debug",
        _ => "trace",
    };
    let filter = EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| EnvFilter::new(format!("uml_launcher={default_level}")));

    let builder = fmt()
        .with_env_filter(filter)
        .with_target(false)
        .with_writer(std::io::stderr);

    match format {
        cli::LogFormat::Plain => builder.init(),
        cli::LogFormat::Json => builder.json().init(),
    }
}
