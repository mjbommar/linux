// SPDX-License-Identifier: GPL-2.0
//
// Minimal UML mconsole v2 client used by umlctl commands.

use anyhow::{bail, Context, Result};
use std::fs;
use std::os::unix::net::UnixDatagram;
use std::path::{Path, PathBuf};
use std::time::Duration;

const MCONSOLE_MAGIC: u32 = 0xcafebabe;
const MCONSOLE_VERSION: u32 = 2;
const MCONSOLE_MAX_DATA: usize = 512;
const MCONSOLE_HEADER_LEN: usize = 12;
const MCONSOLE_PACKET_LEN: usize = MCONSOLE_HEADER_LEN + MCONSOLE_MAX_DATA;
const MCONSOLE_REPLY_LIMIT: usize = 64;
const MCONSOLE_DEFAULT_TIMEOUT: Duration = Duration::from_secs(10);

struct SocketPathGuard(PathBuf);

impl Drop for SocketPathGuard {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

pub fn send_mconsole_command(socket_path: &Path, command: &str) -> Result<String> {
    send_mconsole_command_with_timeout(socket_path, command, MCONSOLE_DEFAULT_TIMEOUT)
}

pub fn send_mconsole_command_with_timeout(
    socket_path: &Path,
    command: &str,
    timeout: Duration,
) -> Result<String> {
    if !socket_path.exists() {
        bail!("mconsole socket {} does not exist", socket_path.display());
    }

    let packet = build_mconsole_request(command)?;
    let client_path = std::env::temp_dir().join(format!(
        "umlctl-mconsole-{}-{}.sock",
        std::process::id(),
        crate::run::generate_run_id()
    ));
    let _ = fs::remove_file(&client_path);
    let _guard = SocketPathGuard(client_path.clone());

    let sock = UnixDatagram::bind(&client_path)
        .with_context(|| format!("bind mconsole client socket {}", client_path.display()))?;
    sock.connect(socket_path)
        .with_context(|| format!("connect mconsole socket {}", socket_path.display()))?;
    sock.set_read_timeout(Some(timeout))
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
