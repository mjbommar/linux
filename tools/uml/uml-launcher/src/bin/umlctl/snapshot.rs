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
use std::os::unix::net::UnixDatagram;
use std::path::{Path, PathBuf};
use std::time::Duration;

use super::manifest::Manifest;
use super::paths::Paths;
use super::run::Run;
use super::supervise;

const MCONSOLE_MAGIC: u32 = 0xcafebabe;
const MCONSOLE_VERSION: u32 = 2;
const MCONSOLE_MAX_DATA: usize = 512;
const MCONSOLE_HEADER_LEN: usize = 12;
const MCONSOLE_PACKET_LEN: usize = MCONSOLE_HEADER_LEN + MCONSOLE_MAX_DATA;
const MCONSOLE_REPLY_LIMIT: usize = 64;

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

struct SocketPathGuard(PathBuf);

impl Drop for SocketPathGuard {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

fn send_mconsole_command(socket_path: &Path, command: &str) -> Result<String> {
    if !socket_path.exists() {
        bail!("mconsole socket {} does not exist", socket_path.display());
    }

    let packet = build_mconsole_request(command)?;
    let client_path = std::env::temp_dir().join(format!(
        "umlctl-mconsole-{}-{}.sock",
        std::process::id(),
        super::run::generate_run_id()
    ));
    let _ = fs::remove_file(&client_path);
    let _guard = SocketPathGuard(client_path.clone());

    let sock = UnixDatagram::bind(&client_path)
        .with_context(|| format!("bind mconsole client socket {}", client_path.display()))?;
    sock.connect(socket_path)
        .with_context(|| format!("connect mconsole socket {}", socket_path.display()))?;
    sock.set_read_timeout(Some(Duration::from_secs(10)))
        .context("set mconsole read timeout")?;
    sock.send(&packet).context("send mconsole request")?;

    let mut out = String::new();
    for _ in 0..MCONSOLE_REPLY_LIMIT {
        let mut reply = [0u8; MCONSOLE_PACKET_LEN];
        let n = sock.recv(&mut reply).context("receive mconsole reply")?;
        if n < MCONSOLE_HEADER_LEN {
            bail!("short mconsole reply: {n} bytes");
        }

        let err = u32::from_ne_bytes(reply[0..4].try_into().unwrap());
        let more = u32::from_ne_bytes(reply[4..8].try_into().unwrap());
        let len = u32::from_ne_bytes(reply[8..12].try_into().unwrap()) as usize;
        let available = n.saturating_sub(MCONSOLE_HEADER_LEN).min(MCONSOLE_MAX_DATA);
        let data_len = len.saturating_sub(1).min(available);
        out.push_str(&String::from_utf8_lossy(
            &reply[MCONSOLE_HEADER_LEN..MCONSOLE_HEADER_LEN + data_len],
        ));

        if err != 0 {
            let msg = out.trim();
            if msg.is_empty() {
                bail!("mconsole command failed");
            }
            bail!("mconsole command failed: {msg}");
        }
        if more == 0 {
            return Ok(out);
        }
    }

    bail!("mconsole reply exceeded {MCONSOLE_REPLY_LIMIT} packets");
}

fn build_mconsole_request(command: &str) -> Result<[u8; MCONSOLE_PACKET_LEN]> {
    let bytes = command.as_bytes();
    if bytes.len() >= MCONSOLE_MAX_DATA {
        bail!(
            "mconsole command too long: {} bytes, max {}",
            bytes.len(),
            MCONSOLE_MAX_DATA - 1
        );
    }

    let mut packet = [0u8; MCONSOLE_PACKET_LEN];
    packet[0..4].copy_from_slice(&MCONSOLE_MAGIC.to_ne_bytes());
    packet[4..8].copy_from_slice(&MCONSOLE_VERSION.to_ne_bytes());
    packet[8..12].copy_from_slice(&(bytes.len() as u32).to_ne_bytes());
    packet[MCONSOLE_HEADER_LEN..MCONSOLE_HEADER_LEN + bytes.len()].copy_from_slice(bytes);
    Ok(packet)
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

    #[test]
    fn mconsole_request_shape() {
        let packet = build_mconsole_request("snapshot_export /tmp/snap.elf").unwrap();
        assert_eq!(
            u32::from_ne_bytes(packet[0..4].try_into().unwrap()),
            MCONSOLE_MAGIC
        );
        assert_eq!(
            u32::from_ne_bytes(packet[4..8].try_into().unwrap()),
            MCONSOLE_VERSION
        );
        assert_eq!(u32::from_ne_bytes(packet[8..12].try_into().unwrap()), 29);
        assert_eq!(
            &packet[MCONSOLE_HEADER_LEN..MCONSOLE_HEADER_LEN + 29],
            b"snapshot_export /tmp/snap.elf"
        );
    }

    #[test]
    fn mconsole_request_rejects_oversized_command() {
        let command = "x".repeat(MCONSOLE_MAX_DATA);
        assert!(build_mconsole_request(&command).is_err());
    }
}
