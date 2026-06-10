// SPDX-License-Identifier: GPL-2.0
//
// umlctl port-forward — return a guest-reachable host address for a
// pool member.
//
// In TAP-direct mode the guest is already wired to the host's TAP
// gateway IP at take time; nothing extra has to happen.  The verb's
// only job is to look up the member's `ipv4_gateway` via the daemon's
// `list` RPC and print a well-typed answer:
//
//   { "ok": true,
//     "schema_version": "port-forward/1",
//     "guest_address": "10.7.0.1:35419",
//     "scheme": "tap-direct",
//     "host_port": 35419,
//     "guest_port": 35419,
//     "pid": 1234 }
//
// iptables-DNAT fallback is intentionally NOT implemented here.  The
// syzkaller shim does not need it in the TAP-direct model.  A future
// caller that needs DNAT can add a `--scheme dnat` branch.

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;

use crate::pool_client;

#[derive(Args, Debug)]
pub struct PortForwardArgs {
    /// Pool name (locates the daemon socket).
    #[arg(long, default_value = "default", value_name = "NAME")]
    pub name: String,

    /// Host pid of the pool member to forward.
    #[arg(long, value_name = "PID")]
    pub pid: i32,

    /// Port the guest will dial back to.  Required.
    #[arg(long, value_name = "HPORT")]
    pub host_port: u16,

    /// Distinct guest-side port.  Defaults to host-port (the common
    /// case for syzkaller's manager-to-executor channel where both
    /// sides agree on a single port number).
    #[arg(long, value_name = "GPORT")]
    pub guest_port: Option<u16>,

    /// Emit a single JSON object instead of a human line.
    #[arg(long)]
    pub json: bool,
}

/// Result struct — public so the syzkaller shim can decode it
/// without re-deriving the schema from prose.
#[derive(serde::Serialize, serde::Deserialize, Debug, PartialEq)]
pub struct ForwardResult {
    pub ok: bool,
    pub schema_version: String,
    pub guest_address: String,
    pub scheme: String,
    pub host_port: u16,
    pub guest_port: u16,
    pub pid: i32,
}

/// Strip an optional CIDR suffix.  `"10.7.0.1/24"` → `"10.7.0.1"`,
/// `"10.7.0.1"` → `"10.7.0.1"`.  Public for tests.
pub fn strip_cidr(addr: &str) -> &str {
    match addr.split_once('/') {
        Some((host, _)) => host,
        None => addr,
    }
}

/// Look up the named member's gateway address.  Public so tests can
/// stub the lookup against a synthetic `list` reply.
pub fn gateway_from_list_reply(reply: &serde_json::Value, pid: i32) -> Result<String> {
    let members = reply
        .get("members")
        .and_then(|v| v.as_array())
        .ok_or_else(|| anyhow!("pool list reply has no `members` array"))?;
    for m in members {
        let mpid = m.get("pid").and_then(|v| v.as_i64()).unwrap_or(-1) as i32;
        if mpid == pid {
            let gw = m.get("ipv4_gateway").and_then(|v| v.as_str()).unwrap_or("");
            if gw.is_empty() {
                bail!(
                    "pool member pid {} has no ipv4_gateway recorded; \
                       was it taken with --gateway?",
                    pid
                );
            }
            return Ok(strip_cidr(gw).to_string());
        }
    }
    bail!("pool member pid {} not found in daemon's member table", pid);
}

/// Build the `ForwardResult` from a member's gateway and the requested
/// port pair.  Pure helper; covers schema versioning + the host==guest
/// port default.  Public for tests.
pub fn build_forward_result(
    gateway: &str,
    pid: i32,
    host_port: u16,
    guest_port: Option<u16>,
) -> ForwardResult {
    let guest_port = guest_port.unwrap_or(host_port);
    ForwardResult {
        ok: true,
        schema_version: "port-forward/1".to_string(),
        guest_address: format!("{}:{}", gateway, host_port),
        scheme: "tap-direct".to_string(),
        host_port,
        guest_port,
        pid,
    }
}

pub fn cmd_port_forward(
    args: PortForwardArgs,
    paths: &crate::paths::Paths,
    quiet: bool,
) -> Result<()> {
    if args.host_port == 0 {
        bail!("--host-port must be non-zero");
    }
    let socket = pool_client::pool_socket_path(&paths.runtime_dir, &args.name);
    if !socket.exists() {
        bail!(
            "no pool daemon at {} (start one with `umlctl pool serve --name {}`)",
            socket.display(),
            args.name
        );
    }

    let req = serde_json::json!({"op": "list"});
    let reply = pool_client::rpc(&socket, &req).context("port-forward list RPC")?;
    let reply = pool_client::unwrap_envelope(reply)?;
    let gateway = gateway_from_list_reply(&reply, args.pid)?;

    let result = build_forward_result(&gateway, args.pid, args.host_port, args.guest_port);
    if args.json {
        let s = serde_json::to_string(&result).context("serialize port-forward result")?;
        println!("{}", s);
    } else if !quiet {
        println!(
            "tap-direct: pid={} host_port={} guest dials {}",
            result.pid, result.host_port, result.guest_address
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strip_cidr_handles_both_shapes() {
        assert_eq!(strip_cidr("10.7.0.1/24"), "10.7.0.1");
        assert_eq!(strip_cidr("10.7.0.1"), "10.7.0.1");
        assert_eq!(strip_cidr(""), "");
    }

    #[test]
    fn gateway_lookup_finds_member() {
        let r = serde_json::json!({
            "ok": true,
            "members": [
                {"pid": 100, "ipv4_gateway": "10.7.0.1/24"},
                {"pid": 200, "ipv4_gateway": "10.7.0.2"},
            ]
        });
        assert_eq!(gateway_from_list_reply(&r, 100).unwrap(), "10.7.0.1");
        assert_eq!(gateway_from_list_reply(&r, 200).unwrap(), "10.7.0.2");
    }

    #[test]
    fn gateway_lookup_missing_pid_errors() {
        let r = serde_json::json!({"members": []});
        assert!(gateway_from_list_reply(&r, 100).is_err());
    }

    #[test]
    fn gateway_lookup_member_without_gateway_errors() {
        let r = serde_json::json!({
            "members": [{"pid": 100, "ipv4_gateway": ""}]
        });
        let err = gateway_from_list_reply(&r, 100).unwrap_err();
        let msg = format!("{:#}", err);
        assert!(msg.contains("no ipv4_gateway"), "got: {}", msg);
    }

    #[test]
    fn build_result_default_guest_port() {
        let r = build_forward_result("10.7.0.1", 99, 35419, None);
        assert_eq!(r.guest_port, 35419);
        assert_eq!(r.host_port, 35419);
        assert_eq!(r.guest_address, "10.7.0.1:35419");
        assert_eq!(r.scheme, "tap-direct");
        assert_eq!(r.schema_version, "port-forward/1");
        assert!(r.ok);
    }

    #[test]
    fn build_result_explicit_guest_port() {
        let r = build_forward_result("10.7.0.1", 99, 35419, Some(22));
        assert_eq!(r.guest_port, 22);
        assert_eq!(r.host_port, 35419);
        assert_eq!(r.guest_address, "10.7.0.1:35419");
    }

    #[test]
    fn build_result_serializes_canonical_json() {
        let r = build_forward_result("10.7.0.1", 99, 35419, None);
        let s = serde_json::to_string(&r).unwrap();
        // Round-trip through a Value so we don't depend on field order.
        let v: serde_json::Value = serde_json::from_str(&s).unwrap();
        assert_eq!(v["ok"], true);
        assert_eq!(v["schema_version"], "port-forward/1");
        assert_eq!(v["scheme"], "tap-direct");
        assert_eq!(v["guest_address"], "10.7.0.1:35419");
        assert_eq!(v["pid"], 99);
    }
}
