// SPDX-License-Identifier: GPL-2.0
//
// `umlctl snapshot` subcommands — user-facing wrappers around the
// kvm-v2 backend's snapshot/export plumbing.
//
// Today this module exposes one verb:
//
//   umlctl snapshot export <name> --output <path>
//
// Which resolves <name> to a running UML guest, then drives the
// guest's debugfs trigger (kvm_v2_snapshot_elf_export_path) to write
// an ELF64-core file at <path>. The host side does NOT itself produce
// the ELF; the kernel is the only thing that has authoritative access
// to the captured KVM state. We just (a) discover the running pid, (b)
// figure out where in the guest's mount namespace the host path is
// visible, and (c) trigger the write via debugfs.
//
// Today's path-discovery shortcut: when the guest mounts the host's
// filesystem at / via rootfstype=hostfs (the common case for the dev
// configs), any absolute host path is also a valid guest path. The
// CLI verb assumes that shape and writes the path verbatim to the
// debugfs trigger; if the guest's namespace differs the user is
// expected to use the more explicit shell-into-guest path. The CLI's
// `--output` flag is a host path either way; we resolve to absolute
// and write it through.
//
// Capture is intentionally not exposed until the kernel side has a
// durable handle that can outlive the debugfs trigger. This command
// only implements the demonstrated restore path.

use anyhow::{bail, Context, Result};
use std::fs::OpenOptions;
use std::io::Write;
use std::path::{Path, PathBuf};

use super::manifest::Manifest;
use super::paths::Paths;
use super::run::Run;
use super::supervise;

/// `umlctl snapshot export` argument bundle.
#[derive(clap::Args, Debug)]
pub struct ExportArgs {
    /// Instance name (matches `umlctl ps`).
    pub name: String,

    /// Destination host path for the dumped ELF64 core file. Made
    /// absolute before being forwarded to the guest debugfs trigger.
    #[arg(short = 'o', long = "output", value_name = "PATH")]
    pub output: PathBuf,

    /// Path inside the guest's mount namespace to the kvm-v2 debugfs
    /// trigger. Override if you've remounted debugfs somewhere else.
    #[arg(
        long = "trigger-path",
        default_value = "/sys/kernel/debug/um/kvm_v2_snapshot_elf_export_path",
        value_name = "PATH"
    )]
    pub trigger_path: PathBuf,

    /// Skip the host-side existence check for `--output` before
    /// triggering the dump. Useful when running this from a sidecar
    /// that mounted the destination filesystem with a different view.
    #[arg(long = "no-precheck")]
    pub no_precheck: bool,
}

/// Implementation of `umlctl snapshot export`.
///
/// Resolves the instance to a live PID + a host-side gateway to the
/// guest's debugfs (we expect debugfs to be host-visible at
/// `/proc/<pid>/root/sys/kernel/debug/...` via the
/// rootfstype=hostfs ergonomics every dev config uses). Writes the
/// caller's `--output` path to the trigger, then verifies that a
/// file with non-zero size showed up at the destination.
pub fn cmd_export(paths: &Paths, args: ExportArgs, quiet: bool) -> Result<()> {
    let (pid, _kernel) = resolve_live(paths, &args.name)?;

    // Canonicalise --output to an absolute path. We don't require it
    // to exist (the guest will create it); we just need a path the
    // guest can resolve.
    let output_abs = canonicalise_for_guest(&args.output)?;

    // Reach the guest's debugfs through /proc/<pid>/root, which is
    // the host-visible view of the guest's mount namespace. This
    // works because UML runs as a host process — the same property
    // umlctl strace / gdb / bpf already exploits.
    let trigger_host_path = PathBuf::from(format!("/proc/{pid}/root")).join(
        args.trigger_path
            .strip_prefix("/")
            .unwrap_or(&args.trigger_path),
    );

    if !args.no_precheck && !trigger_host_path.exists() {
        bail!(
            "snapshot debugfs trigger {} not visible from host (CONFIG_DEBUG_FS off, \
             guest not mounted debugfs, or kvm-v2 backend not selected)",
            trigger_host_path.display()
        );
    }

    if !quiet {
        eprintln!(
            "[umlctl] snapshot export: instance='{}' pid={} → {}",
            args.name,
            pid,
            output_abs.display()
        );
    }

    // Open the trigger O_WRONLY and write the path. debugfs takes the
    // whole write atomically; one syscall = one trigger.
    let mut f = OpenOptions::new()
        .write(true)
        .open(&trigger_host_path)
        .with_context(|| {
            format!(
                "open debugfs trigger {} for write",
                trigger_host_path.display()
            )
        })?;
    let line = format!("{}\n", output_abs.display());
    f.write_all(line.as_bytes()).with_context(|| {
        format!(
            "write to debugfs trigger {} (path={})",
            trigger_host_path.display(),
            output_abs.display()
        )
    })?;
    drop(f);

    // The kernel's writer is synchronous from debugfs_write's
    // perspective — by the time the write(2) returns, the ELF is on
    // disk. Verify.
    let meta = std::fs::metadata(&output_abs).with_context(|| {
        format!(
            "no file at {} after trigger — did the kernel writer fail?",
            output_abs.display()
        )
    })?;
    if meta.len() == 0 {
        bail!(
            "snapshot export wrote zero bytes to {} (kernel logged a warning?)",
            output_abs.display()
        );
    }

    if !quiet {
        eprintln!(
            "[umlctl] snapshot export: wrote {} bytes to {}",
            meta.len(),
            output_abs.display()
        );
    }
    Ok(())
}

/// Resolve `<name>` to (pid, kernel_path). Same shape as
/// transparency::resolve_live but inlined here so the dependency goes
/// one direction.
fn resolve_live(paths: &Paths, name: &str) -> Result<(u32, PathBuf)> {
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
    Ok((run.pid, manifest.kernel.path.clone()))
}

/// Canonicalise a host path for forwarding to the guest. The path
/// doesn't need to exist (the guest creates it on write) but must
/// resolve to an absolute path so the guest doesn't end up writing
/// to its own /. Prepends the host CWD when relative.
fn canonicalise_for_guest(p: &Path) -> Result<PathBuf> {
    if p.is_absolute() {
        return Ok(p.to_path_buf());
    }
    let cwd = std::env::current_dir().context("getcwd")?;
    Ok(cwd.join(p))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonicalise_absolute_passes_through() {
        let p = Path::new("/tmp/foo.elf");
        assert_eq!(canonicalise_for_guest(p).unwrap(), p);
    }

    #[test]
    fn canonicalise_relative_anchors_to_cwd() {
        let cwd = std::env::current_dir().unwrap();
        let p = Path::new("snap.elf");
        let abs = canonicalise_for_guest(p).unwrap();
        assert!(abs.is_absolute());
        assert_eq!(abs, cwd.join("snap.elf"));
    }
}
