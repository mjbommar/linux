// SPDX-License-Identifier: GPL-2.0
//
// umlctl pool take / pool status — client-side wrappers for the
// `pool serve` daemon's `take` and `status` RPCs.
//
// Why a thin client verb and not "let the Go shim speak the socket":
// keeping one Rust source of truth for the wire format avoids the
// classic vendor-integration cost where every caller re-implements
// the JSON envelope and drifts.  The syzkaller shim shells out to
// these verbs; users can also drive them from the shell.

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;

use crate::pool_client;

#[derive(Args, Debug)]
pub struct TakeArgs {
    /// Pool name (locates the daemon socket).
    #[arg(long, default_value = "default", value_name = "NAME")]
    pub name: String,

    /// Logical instance name written into the identity blob.
    /// Auto-synthesized from --index if omitted.
    #[arg(long, value_name = "NAME")]
    pub instance: Option<String>,

    /// Index used to auto-synthesize --instance and the MAC tail.
    /// Matches syzkaller's `Create(workdir, index)` slot index.
    #[arg(long, default_value_t = 0, value_name = "N")]
    pub index: u32,

    /// MAC address (default: 52:54:00:00:00:<index> LSB).
    #[arg(long, value_name = "MAC")]
    pub mac: Option<String>,

    /// TAP device name.
    #[arg(long, default_value = "", value_name = "NAME")]
    pub tap: String,

    /// IPv4 CIDR (e.g. 10.7.0.42/24).
    #[arg(long, default_value = "", value_name = "CIDR")]
    pub ipv4: String,

    /// IPv4 gateway.
    #[arg(long, default_value = "", value_name = "ADDR")]
    pub gateway: String,

    /// Mconsole socket path.  Empty = let the daemon synthesize one
    /// under the per-pool runtime dir.
    #[arg(long, default_value = "", value_name = "PATH")]
    pub mconsole: String,

    /// Emit the daemon's reply as a single JSON object.
    #[arg(long)]
    pub json: bool,
}

#[derive(Args, Debug)]
pub struct StatusArgs {
    /// Pool name.
    #[arg(long, default_value = "default", value_name = "NAME")]
    pub name: String,

    /// Emit as JSON.
    #[arg(long)]
    pub json: bool,
}

/// Default per-index MAC.  Public for tests.
pub fn default_mac(index: u32) -> String {
    let lo = (index & 0xff) as u8;
    let mid = ((index >> 8) & 0xff) as u8;
    format!("52:54:00:00:{:02x}:{:02x}", mid, lo)
}

/// Default per-index instance name.  Public for tests.
pub fn default_instance(index: u32) -> String {
    format!("uml-{}", index)
}

pub fn cmd_take(args: TakeArgs, paths: &crate::paths::Paths, quiet: bool) -> Result<()> {
    let socket = pool_client::pool_socket_path(&paths.runtime_dir, &args.name);
    if !socket.exists() {
        bail!(
            "no pool daemon at {} (start one with `umlctl pool serve --name {}`)",
            socket.display(),
            args.name
        );
    }
    let instance = args
        .instance
        .clone()
        .unwrap_or_else(|| default_instance(args.index));
    let mac = args.mac.clone().unwrap_or_else(|| default_mac(args.index));

    let req = serde_json::json!({
        "op": "take",
        "instance": instance,
        "mac": mac,
        "tap": args.tap,
        "ipv4": args.ipv4,
        "gateway": args.gateway,
        "mconsole": args.mconsole,
    });
    let reply = pool_client::rpc(&socket, &req).context("take RPC")?;
    let reply = pool_client::unwrap_envelope(reply)?;
    let result = reply
        .get("result")
        .ok_or_else(|| anyhow!("daemon take reply missing `result` field: {}", reply))?;

    if args.json {
        // Emit the SpawnResult-shaped object directly so callers don't
        // have to unwrap the envelope a second time.
        println!("{}", result);
    } else if !quiet {
        let pid = result.get("pid").and_then(|v| v.as_i64()).unwrap_or(0);
        let inst = result
            .get("instance")
            .and_then(|v| v.as_str())
            .unwrap_or("");
        let ipv4 = result
            .get("ipv4_cidr")
            .and_then(|v| v.as_str())
            .unwrap_or("");
        println!("took pid={} instance={} ipv4={}", pid, inst, ipv4);
    }
    Ok(())
}

pub fn cmd_status(args: StatusArgs, paths: &crate::paths::Paths, quiet: bool) -> Result<()> {
    let socket = pool_client::pool_socket_path(&paths.runtime_dir, &args.name);
    if !socket.exists() {
        bail!(
            "no pool daemon at {} (start one with `umlctl pool serve --name {}`)",
            socket.display(),
            args.name
        );
    }
    let req = serde_json::json!({"op": "status"});
    let reply = pool_client::rpc(&socket, &req).context("status RPC")?;
    let reply = pool_client::unwrap_envelope(reply)?;
    if args.json {
        println!("{}", reply);
    } else if !quiet {
        let master = reply
            .get("master_pid")
            .and_then(|v| v.as_i64())
            .unwrap_or(0);
        let taken = reply.get("taken").and_then(|v| v.as_i64()).unwrap_or(0);
        let sock = reply.get("socket").and_then(|v| v.as_str()).unwrap_or("");
        println!(
            "pool={} master_pid={} taken={} socket={}",
            args.name, master, taken, sock
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_mac_per_index() {
        assert_eq!(default_mac(0), "52:54:00:00:00:00");
        assert_eq!(default_mac(1), "52:54:00:00:00:01");
        assert_eq!(default_mac(0x12fe), "52:54:00:00:12:fe");
        // Stay deterministic: index 256 wraps into the mid byte.
        assert_eq!(default_mac(256), "52:54:00:00:01:00");
    }

    #[test]
    fn default_instance_per_index() {
        assert_eq!(default_instance(0), "uml-0");
        assert_eq!(default_instance(42), "uml-42");
    }
}
