// SPDX-License-Identifier: GPL-2.0
//
// umlbuild — reproducible UML instance builder.
//
// Sibling of umlctl: umlctl runs an already-built UML instance;
// umlbuild *constructs* one. End-to-end:
//
//   umlbuild instance --profile mvp --out ~/uml-mvp
//   umlctl up -f ~/uml-mvp/Umlfile.toml
//
// See src/bin/umlbuild/SPEC.md for the design rationale.
//
// v1 verbs: kernel, rootfs, image, instance, profile.
// Same conventions as umlctl: clap-derive, anyhow::Result,
// tracing, eprintln!("umlbuild: …") on exit, exit(1) on Err.

use anyhow::Result;
use clap::{Parser, Subcommand};

mod image;
mod instance;
mod kernel;
mod paths;
mod profile;
mod rootfs;
mod shell;

/// Top-level `umlbuild` invocation.
#[derive(Parser, Debug)]
#[command(
    name = "umlbuild",
    version,
    about = "Reproducible UML instance builder (kernel + rootfs + ubd image).",
    long_about = "umlbuild produces the inputs umlctl consumes: a UML kernel \
binary, a ubd-attachable rootfs image, and an Umlfile.toml that wires them \
together. Each subcommand is independently runnable; `umlbuild instance` is \
the end-to-end orchestrator.\n\n\
Profiles are TOML files describing what to build (kernel Kconfig overlay, \
rootfs base + packages, image size). Built-in profiles ship under \
tools/uml/uml-launcher/profiles/; user profiles live under \
$XDG_CONFIG_HOME/uml-build/profiles/."
)]
struct Cli {
    /// Verbosity. -v: info, -vv: debug, -vvv: trace.
    #[arg(short, long, action = clap::ArgAction::Count, global = true)]
    verbose: u8,

    /// Log format. `plain` (default) for human, `json` for machine.
    #[arg(long, value_name = "FMT", default_value = "plain", global = true)]
    log_format: String,

    #[command(subcommand)]
    cmd: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Build a UML kernel binary from a profile's Kconfig overlay.
    Kernel(kernel::KernelArgs),

    /// Construct a populated rootfs directory tree (alpine or debian-slim).
    Rootfs(rootfs::RootfsArgs),

    /// Pack a rootfs directory into a ubd-attachable ext4 image (no sudo).
    Image(image::ImageArgs),

    /// End-to-end: kernel + rootfs + image + emitted Umlfile.toml.
    Instance(instance::InstanceArgs),

    /// Build (if needed) + drop into an interactive shell or REPL
    /// inside the freshly-booted guest.  Docker-shaped UX:
    /// `umlbuild shell --profile sandbox --cmd /usr/bin/python3`.
    Shell(shell::ShellArgs),

    /// List or show profiles.
    #[command(subcommand)]
    Profile(profile::ProfileCmd),
}

fn main() {
    let cli = Cli::parse();

    init_tracing(cli.verbose, &cli.log_format);

    let result: Result<()> = match cli.cmd {
        Command::Kernel(args) => kernel::run(args),
        Command::Rootfs(args) => rootfs::run(args),
        Command::Image(args) => image::run(args),
        Command::Instance(args) => instance::run(args),
        Command::Shell(args) => shell::run(args),
        Command::Profile(cmd) => profile::run(cmd),
    };

    if let Err(e) = result {
        eprintln!("umlbuild: {e:#}");
        std::process::exit(1);
    }
}

fn init_tracing(verbose: u8, format: &str) {
    use tracing_subscriber::{fmt, EnvFilter};

    // CLI flag wins over RUST_LOG.
    let level = match verbose {
        0 => "warn",
        1 => "info",
        2 => "debug",
        _ => "trace",
    };
    let filter = EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| EnvFilter::new(format!("umlbuild={level},warn")));

    let builder = fmt().with_env_filter(filter).with_target(false);
    match format {
        "json" => {
            builder.json().init();
        }
        _ => {
            builder.init();
        }
    }
}
