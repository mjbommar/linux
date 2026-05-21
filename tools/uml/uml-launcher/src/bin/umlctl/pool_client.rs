// SPDX-License-Identifier: GPL-2.0
//
// umlctl pool client — Unix-socket RPC client for the `umlctl pool
// serve` daemon (Memo 09 Phase 4).
//
// The daemon (pool_serve.rs) exposes a one-request-per-connection
// JSON-line protocol on $XDG_RUNTIME_DIR/uml/pools/<name>/api.sock.
// This module centralises three concerns the new client-side verbs
// (`pool take`, `pool status`, `pool destroy --name`, `exec`,
// `port-forward`) all share:
//
//   1. compute the socket path from a pool name,
//   2. send one JSON line + read one JSON line back,
//   3. unwrap the {"ok":true|false,...} envelope into a Result.
//
// Why a single module: the syzkaller shim spec (memo 11 §3.3)
// explicitly chose option (b) — `umlctl pool take` etc. wrap the
// daemon RPC rather than each Go caller speaking the wire format.
// Keeping the wire shape in one Rust place protects that choice.

use anyhow::{anyhow, bail, Context, Result};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::Duration;

/// Compute `$RUNTIME_DIR/pools/<name>/api.sock`.  Mirrors the layout
/// `pool_serve.rs::pool_socket` writes.  Kept private here so the
/// daemon module remains the source of truth for layout details; we
/// just duplicate the path computation to avoid pulling the entire
/// pool_serve module into the client.
pub fn pool_socket_path(runtime_dir: &Path, name: &str) -> PathBuf {
    runtime_dir.join("pools").join(name).join("api.sock")
}

/// One JSON RPC against the daemon.  Sends `req` as a single line,
/// reads one line of reply, parses it.  Returns the entire reply
/// object (callers downcast `ok`, `result`, `error` etc.).
pub fn rpc(socket: &Path, req: &serde_json::Value) -> Result<serde_json::Value> {
    let stream = UnixStream::connect(socket).with_context(|| {
        format!(
            "connect to pool daemon at {} (is `umlctl pool serve --name <pool>` running?)",
            socket.display()
        )
    })?;
    stream.set_read_timeout(Some(Duration::from_secs(60))).ok();
    stream.set_write_timeout(Some(Duration::from_secs(5))).ok();

    let mut writer = stream.try_clone().context("dup UnixStream for write")?;
    let serialized = serde_json::to_string(req).context("serialize RPC request")?;
    writeln!(writer, "{}", serialized).context("write RPC request")?;
    writer.flush().ok();
    drop(writer);

    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    reader.read_line(&mut line).context("read RPC reply")?;
    if line.trim().is_empty() {
        bail!("pool daemon returned empty reply (daemon crashed?)");
    }
    serde_json::from_str::<serde_json::Value>(line.trim())
        .with_context(|| format!("parse RPC reply {:?}", line.trim()))
}

/// Unwrap the standard `{"ok":true,...}` / `{"ok":false,"error":...}`
/// envelope.  Returns the full object on success so the caller can
/// pull out fields like `result`, `members`, `master_pid`, …
pub fn unwrap_envelope(reply: serde_json::Value) -> Result<serde_json::Value> {
    match reply.get("ok").and_then(|v| v.as_bool()) {
        Some(true) => Ok(reply),
        Some(false) => {
            let msg = reply
                .get("error")
                .and_then(|v| v.as_str())
                .unwrap_or("pool daemon reported failure without an error message");
            Err(anyhow!("pool daemon RPC failed: {}", msg))
        }
        None => Err(anyhow!(
            "pool daemon reply is missing the `ok` field: {}",
            reply
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn socket_path_layout() {
        let p = Path::new("/run/uml");
        assert_eq!(
            pool_socket_path(p, "default"),
            PathBuf::from("/run/uml/pools/default/api.sock")
        );
        assert_eq!(
            pool_socket_path(p, "syz-pool"),
            PathBuf::from("/run/uml/pools/syz-pool/api.sock")
        );
    }

    #[test]
    fn envelope_ok_passes_through() {
        let v = serde_json::json!({"ok": true, "result": {"pid": 42}});
        let out = unwrap_envelope(v.clone()).unwrap();
        assert_eq!(out, v);
    }

    #[test]
    fn envelope_error_extracted() {
        let v = serde_json::json!({"ok": false, "error": "no such pid"});
        let err = unwrap_envelope(v).unwrap_err();
        let msg = format!("{:#}", err);
        assert!(msg.contains("no such pid"), "got: {}", msg);
    }

    #[test]
    fn envelope_missing_ok_is_error() {
        let v = serde_json::json!({"weird": true});
        assert!(unwrap_envelope(v).is_err());
    }

    #[test]
    fn envelope_false_no_error_field_still_fails() {
        // The daemon should always emit an `error` string when
        // ok=false, but be resilient if a future version forgets to.
        let v = serde_json::json!({"ok": false});
        let err = unwrap_envelope(v).unwrap_err();
        let msg = format!("{:#}", err);
        assert!(msg.contains("without an error message"), "got: {}", msg);
    }
}
