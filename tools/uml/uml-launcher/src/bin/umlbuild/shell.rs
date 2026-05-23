// SPDX-License-Identifier: GPL-2.0
//
// umlbuild shell — docker-style "drop into an interactive prompt inside
// the freshly-built instance."  Wraps `umlbuild instance` + a kernel
// launch with stdio wired to the user's terminal.
//
// Examples:
//
//   umlbuild shell                          # mvp profile, /bin/sh
//   umlbuild shell --profile sandbox
//   umlbuild shell --profile dev --cmd /bin/bash
//   umlbuild shell --profile mvp --cmd /usr/bin/python3   # Python REPL
//
// Unlike `umlctl up` (which supervises the kernel as a child and
// captures stdio to log files), `umlbuild shell` execve's into the
// kernel so the host TTY *is* the guest's console — Ctrl-C, line
// editing, REPL prompts all work.

use anyhow::Result;
use clap::Args;
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::Command;

use crate::{instance, profile};

#[derive(Args, Debug)]
pub struct ShellArgs {
    /// Profile name or path to a profile TOML.
    #[arg(long, value_name = "NAME_OR_PATH", default_value = "mvp")]
    pub profile: String,

    /// Instance directory.  Built (or rebuilt with --force) if missing.
    /// Defaults to a temp dir under $XDG_CACHE_HOME/uml-build/shells/.
    #[arg(long, value_name = "DIR")]
    pub out: Option<PathBuf>,

    /// Override the init command run inside the guest.  Default:
    /// "/bin/sh".  Common picks: "/bin/bash", "/usr/bin/python3",
    /// "/usr/bin/python3 -i" for REPL with -i, "/bin/ash".
    #[arg(long, value_name = "PATH", default_value = "/bin/sh")]
    pub cmd: String,

    /// Memory size override (defaults to the profile's instance.mem).
    #[arg(long, value_name = "SIZE")]
    pub mem: Option<String>,

    /// Force rebuild even if the instance dir already exists.
    #[arg(long)]
    pub force: bool,

    /// Source-tree path (forwarded to `umlbuild kernel`).
    #[arg(long, value_name = "PATH")]
    pub source: Option<PathBuf>,
}

pub fn run(args: ShellArgs) -> Result<()> {
    let prof = profile::resolve(&args.profile)?;

    // Default the instance out to a profile-keyed dir under XDG cache.
    let out = match args.out {
        Some(p) => p,
        None => {
            let cache = crate::paths::Paths::resolve()?;
            cache.cache_root.join("shells").join(&prof.profile.name)
        }
    };

    // Build / reuse the instance.
    let need_build = args.force
        || !out.join("linux").is_file()
        || !out.join("rootfs.img").is_file();
    if need_build {
        eprintln!(
            "umlbuild shell: building instance at {} (use --out to override)",
            out.display()
        );
        instance::run(instance::InstanceArgs {
            profile: args.profile.clone(),
            out: out.clone(),
            source: args.source.clone(),
            force: args.force,
        })?;
    } else {
        eprintln!(
            "umlbuild shell: reusing instance at {} (pass --force to rebuild)",
            out.display()
        );
    }

    let kernel = out.join("linux");
    let image = out.join("rootfs.img");
    if !kernel.is_file() || !image.is_file() {
        anyhow::bail!(
            "instance at {} is missing linux or rootfs.img after build",
            out.display()
        );
    }

    let mem = args.mem.unwrap_or_else(|| prof.instance.mem.clone());

    // The cmd may have space-separated args ("/usr/bin/python3 -i").
    // Split on whitespace; first token becomes init=PATH, the rest go
    // after the kernel cmdline.  The kernel passes "unknown" tokens to
    // userspace as argv to init.
    let mut cmd_tokens = args
        .cmd
        .split_whitespace()
        .map(String::from)
        .collect::<Vec<_>>();
    let init_path = cmd_tokens
        .drain(..1)
        .next()
        .unwrap_or_else(|| "/bin/sh".to_string());

    let mut argv: Vec<String> = vec![
        format!("mem={mem}"),
        format!("ubd0={}", image.display()),
        "root=/dev/ubda".into(),
        "rw".into(),
        // fd:0 = stdin from launching terminal, fd:1 = stdout to it.
        // Format: con=IN,OUT (so con=fd:0,fd:1 means stdin from host
        // fd 0 + stdout to host fd 1).  All secondary consoles get
        // routed through null,fd:1 so they don't try to grab stdin.
        "con=fd:0,fd:1".into(),
        "con0=fd:0,fd:1".into(),
        "con1=null,fd:1".into(),
        // Override the rootfs's /sbin/init with the requested cmd.
        format!("init={init_path}"),
    ];
    // Extra cmd args go on the cmdline — kernel forwards unrecognized
    // tokens to userspace as init's argv.
    argv.extend(cmd_tokens);

    eprintln!("umlbuild shell: launching {} init={init_path}", kernel.display());
    eprintln!("                Ctrl-D or `exit` to leave; kernel will power down.");
    eprintln!();

    // exec replaces the umlbuild process with the kernel, so the TTY
    // is owned by the kernel + guest userspace directly (Ctrl-C, line
    // editing, REPL prompts all work).
    let err = Command::new(&kernel)
        .args(&argv)
        .exec();
    Err(anyhow::Error::new(err).context(format!("execve {}", kernel.display())))
}
