// SPDX-License-Identifier: GPL-2.0
//
// `umlctl snapshot` subcommands - user-facing wrappers around the
// kvm-v2 backend's snapshot/export plumbing.
//
// Today this module exposes one verb:
//
//   umlctl snapshot export <name> --output <path>
//
// Which resolves <name> to a running UML guest, then asks the kernel's
// mconsole `snapshot_export` command to write an ELF64-core file at
// <path>. The host side does NOT itself produce the ELF; the kernel is
// the only thing that has authoritative access to the captured KVM
// state. We just discover the running pid, find the mconsole socket
// recorded in the run log, and send one bounded datagram command.
//
// The mconsole handler opens the output through UML host-file helpers, so
// `--output` is a host path. The direct debugfs interface remains available
// for scripts running inside the guest.
//
// Capture is intentionally not exposed until the kernel side has a
// durable handle that can outlive the debugfs trigger. This command
// only implements the demonstrated export path.

use anyhow::{bail, Context, Result};
use std::fs;
use std::path::{Path, PathBuf};

use super::manifest::Manifest;
use super::mconsole_client::send_mconsole_command;
use super::paths::Paths;
use super::run::Run;
use super::supervise;

/// `umlctl snapshot export` argument bundle.
#[derive(clap::Args, Debug)]
pub struct ExportArgs {
    /// Instance name (matches `umlctl ps`).
    pub name: String,

    /// Destination host path for the dumped ELF64 core file. Made
    /// absolute before being forwarded to mconsole.
    #[arg(short = 'o', long = "output", value_name = "PATH")]
    pub output: PathBuf,
}

/// Implementation of `umlctl snapshot export`.
///
/// Resolves the instance to a live PID + current run bundle, sends the
/// kernel's mconsole snapshot command, then verifies that a file with
/// non-zero size showed up at the destination.
pub fn cmd_export(paths: &Paths, args: ExportArgs, quiet: bool) -> Result<()> {
    let (pid, _kernel, run_id) = resolve_live(paths, &args.name)?;

    // Canonicalise --output to an absolute host path. We don't require it
    // to exist because the kernel creates/truncates it during export.
    let output_abs = canonicalise_for_host(&args.output)?;
    let mconsole_path = resolve_mconsole_path(paths, &run_id)?;

    if !quiet {
        eprintln!(
            "[umlctl] snapshot export: instance='{}' pid={} mconsole={} -> {}",
            args.name,
            pid,
            mconsole_path.display(),
            output_abs.display()
        );
    }

    let command = format!("snapshot_export {}", output_abs.display());
    let reply = send_mconsole_command(&mconsole_path, &command).with_context(|| {
        format!(
            "send mconsole snapshot_export to {}",
            mconsole_path.display()
        )
    })?;

    // The kernel's mconsole handler exports synchronously. By the
    // time the reply is received, the ELF should be on disk.
    let meta = fs::metadata(&output_abs)
        .with_context(|| format!("no file at {} after mconsole export", output_abs.display()))?;
    if meta.len() == 0 {
        bail!(
            "snapshot export wrote zero bytes to {} (kernel logged a warning?)",
            output_abs.display()
        );
    }

    if !quiet {
        let reply = reply.trim();
        if !reply.is_empty() {
            eprintln!("[umlctl] snapshot export: kernel reply: {reply}");
        }
        eprintln!(
            "[umlctl] snapshot export: wrote {} bytes to {}",
            meta.len(),
            output_abs.display()
        );
    }
    Ok(())
}

/// Resolve `<name>` to (pid, kernel_path, run_id). Same shape as
/// transparency::resolve_live but inlined here so the dependency goes
/// one direction.
fn resolve_live(paths: &Paths, name: &str) -> Result<(u32, PathBuf, String)> {
    let manifest_path = paths.manifest_path(name);
    if !manifest_path.exists() {
        bail!("instance '{name}' not found");
    }
    let manifest = Manifest::read(&manifest_path)
        .with_context(|| format!("read manifest for instance '{name}'"))?;
    let run_id = supervise::read_run_id_file(&paths.run_id_file_path(name)).with_context(|| {
        format!("no live run for instance '{name}' — has umlctl up been called?")
    })?;
    let run = Run::read(&paths.run_dir(&run_id).join("run.json"))
        .with_context(|| format!("read run.json for run {run_id}"))?;
    Ok((run.pid, manifest.kernel.path.clone(), run_id))
}

/// Canonicalise a host path for forwarding to mconsole. The path does not
/// need to exist, but it must be absolute so the result does not depend on
/// the UML kernel's working directory.
fn canonicalise_for_host(p: &Path) -> Result<PathBuf> {
    if p.is_absolute() {
        return Ok(p.to_path_buf());
    }
    let cwd = std::env::current_dir().context("getcwd")?;
    Ok(cwd.join(p))
}

fn resolve_mconsole_path(paths: &Paths, run_id: &str) -> Result<PathBuf> {
    let log_path = paths.run_dir(run_id).join("init.log");
    let log =
        fs::read_to_string(&log_path).with_context(|| format!("read {}", log_path.display()))?;
    parse_mconsole_path(&log).with_context(|| {
        format!(
            "mconsole socket path not found in {}; the guest may not have reached mconsole init",
            log_path.display()
        )
    })
}

fn parse_mconsole_path(log: &str) -> Option<PathBuf> {
    log.lines().rev().find_map(|line| {
        if !line.contains("mconsole (version") {
            return None;
        }
        let (_, path) = line.rsplit_once(" initialized on ")?;
        let path = path.trim();
        if path.is_empty() {
            None
        } else {
            Some(PathBuf::from(path))
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonicalise_absolute_passes_through() {
        let p = Path::new("/tmp/foo.elf");
        assert_eq!(canonicalise_for_host(p).unwrap(), p);
    }

    #[test]
    fn canonicalise_relative_anchors_to_cwd() {
        let cwd = std::env::current_dir().unwrap();
        let p = Path::new("snap.elf");
        let abs = canonicalise_for_host(p).unwrap();
        assert!(abs.is_absolute());
        assert_eq!(abs, cwd.join("snap.elf"));
    }

    #[test]
    fn parse_mconsole_path_from_log() {
        let log = "boot\nmconsole (version 2) initialized on /tmp/uml/mconsole\nready\n";
        assert_eq!(
            parse_mconsole_path(log).unwrap(),
            PathBuf::from("/tmp/uml/mconsole")
        );
    }
}
