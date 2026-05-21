// SPDX-License-Identifier: GPL-2.0
//
// umlctl exec — run a command inside a running pool member (Memo 09
// Phase 4, spec memo 11 §3.1).
//
// `umlctl exec --pid <PID> [--name <pool>] [--timeout SECS]
//              [--json] [--env K=V]… [--cwd PATH]
//              -- ARGV...`
//
// Wire shape: send a single `{"op":"exec",...}` JSON RPC to the
// daemon's Unix socket; receive an NDJSON stream framed per spec §3.4:
//
//   {"type":"start", "schema_version":"exec/1", "pid":N,
//    "argv":[…], "cwd":"…", "ts_ns":...}
//   {"type":"stdout", "data":"…"}
//   {"type":"stderr", "data":"…"}
//   {"type":"console", "data":"…"}     // optional
//   {"type":"exit",    "code":N, "signal":S, "duration_ms":D,
//                      "timed_out":false}
//
// In the daemon's MVP backend the in-guest exec primitive is
// mconsole's `exec` verb (synthesized per-take when the caller did
// not supply --mconsole; see pool_serve.rs::synthesize_mconsole_path).
// If the daemon's `exec` RPC returns ok=false, we surface that as a
// `{"type":"exit","code":<N>,"timed_out":false}` frame plus a non-zero
// process exit so syzkaller's harness sees a clean failure.
//
// Why a daemon RPC and not a fork+exec from the umlctl process: the
// pool member is the daemon's grandchild via the master kernel; the
// daemon already holds the mconsole socket path and the per-member
// identity bookkeeping.  Pushing the exec through the daemon keeps
// the shim a thin wrapper.

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;
use std::io::Write;
use std::time::Instant;

use crate::pool_client;

/// `umlctl exec` argument shape.  Designed to be a stable contract for
/// the syzkaller Go shim (`vm/uml/uml.go::Run`); fields here are NOT
/// reordered without a `schema_version` bump.
#[derive(Args, Debug)]
pub struct ExecArgs {
    /// Pool name (locates the daemon socket).
    #[arg(long, default_value = "default", value_name = "NAME")]
    pub name: String,

    /// Host pid of the pool member to exec into (returned by
    /// `umlctl pool take`).
    #[arg(long, value_name = "PID")]
    pub pid: i32,

    /// Wall-clock timeout in seconds.  0 means no timeout.
    #[arg(long, default_value_t = 0, value_name = "SECS")]
    pub timeout: u64,

    /// Emit NDJSON frames per memo 11 §3.4 instead of passing the
    /// guest's stdout/stderr through transparently.  The syzkaller
    /// shim sets this; an operator invoking `umlctl exec` for a quick
    /// `ls /` usually does not.
    #[arg(long)]
    pub json: bool,

    /// Optional working directory inside the guest.
    #[arg(long, value_name = "PATH")]
    pub cwd: Option<String>,

    /// Set environment variables in the guest.  May be repeated.
    /// Format: `KEY=VALUE`.
    #[arg(long = "env", value_name = "K=V")]
    pub envs: Vec<String>,

    /// The argv to execute in the guest.  Required; passed verbatim.
    #[arg(last = true, required = true)]
    pub argv: Vec<String>,
}

/// Build the RPC payload the daemon expects.  Public for tests.
pub fn build_exec_request(args: &ExecArgs) -> Result<serde_json::Value> {
    if args.argv.is_empty() {
        bail!("exec requires at least one argv element after `--`");
    }
    let mut env_map = serde_json::Map::new();
    for kv in &args.envs {
        let (k, v) = kv
            .split_once('=')
            .ok_or_else(|| anyhow!("--env entry {:?} is not KEY=VALUE", kv))?;
        env_map.insert(k.to_string(), serde_json::Value::String(v.to_string()));
    }
    Ok(serde_json::json!({
        "op": "exec",
        "pid": args.pid,
        "argv": args.argv,
        "env": env_map,
        "cwd": args.cwd.clone().unwrap_or_default(),
        "timeout_secs": args.timeout,
    }))
}

/// Output frame as parsed back from the daemon's reply.  The daemon
/// today returns ONE envelope per RPC (it does not currently stream);
/// we synthesize NDJSON frames from a single envelope so the wire
/// shape the syzkaller shim sees is stable across future daemons that
/// DO stream.  See spec memo 11 §3.4 for the canonical frame shape.
#[derive(Debug)]
pub struct ExecOutcome {
    pub stdout: String,
    pub stderr: String,
    pub console: String,
    pub exit_code: i32,
    pub signal: i32,
    pub timed_out: bool,
    pub duration_ms: u64,
    pub error: Option<String>,
}

/// Decode the daemon's reply.  Public for tests.
pub fn decode_exec_reply(reply: &serde_json::Value) -> Result<ExecOutcome> {
    // ok=false → produce an outcome with a synthetic non-zero exit so
    // the caller sees a stable exit-frame, AND with the daemon's
    // error string in .error so JSON mode can surface it.
    let ok = reply.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
    if !ok {
        let err = reply
            .get("error")
            .and_then(|v| v.as_str())
            .unwrap_or("pool daemon returned ok=false without an error message");
        return Ok(ExecOutcome {
            stdout: String::new(),
            stderr: String::new(),
            console: String::new(),
            exit_code: 1,
            signal: 0,
            timed_out: false,
            duration_ms: 0,
            error: Some(err.to_string()),
        });
    }
    let stdout = reply
        .get("stdout")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let stderr = reply
        .get("stderr")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let console = reply
        .get("console")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let exit_code = reply.get("exit").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
    let signal = reply.get("signal").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
    let timed_out = reply
        .get("timed_out")
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    let duration_ms = reply
        .get("duration_ms")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    Ok(ExecOutcome {
        stdout,
        stderr,
        console,
        exit_code,
        signal,
        timed_out,
        duration_ms,
        error: None,
    })
}

/// Emit NDJSON frames in the shape spec memo 11 §3.4 documents.
/// Public for tests so we can assert frame ordering.
pub fn emit_ndjson_frames<W: Write>(
    out: &mut W,
    args: &ExecArgs,
    outcome: &ExecOutcome,
    started_ns: u128,
) -> Result<()> {
    let start = serde_json::json!({
        "type": "start",
        "schema_version": "exec/1",
        "pid": args.pid,
        "argv": args.argv,
        "cwd": args.cwd.clone().unwrap_or_default(),
        "ts_ns": started_ns as u64,
    });
    writeln!(out, "{}", start)?;
    if !outcome.stdout.is_empty() {
        for chunk in chunked(&outcome.stdout, 60_000) {
            let f = serde_json::json!({"type": "stdout", "data": chunk});
            writeln!(out, "{}", f)?;
        }
    }
    if !outcome.stderr.is_empty() {
        for chunk in chunked(&outcome.stderr, 60_000) {
            let f = serde_json::json!({"type": "stderr", "data": chunk});
            writeln!(out, "{}", f)?;
        }
    }
    if !outcome.console.is_empty() {
        for chunk in chunked(&outcome.console, 60_000) {
            let f = serde_json::json!({"type": "console", "data": chunk});
            writeln!(out, "{}", f)?;
        }
    }
    if let Some(err) = &outcome.error {
        // Surface daemon errors as a stderr-typed frame so the
        // syzkaller-side merger sees them without parsing a new type.
        let f = serde_json::json!({
            "type": "stderr",
            "data": format!("umlctl exec: daemon error: {}\n", err),
        });
        writeln!(out, "{}", f)?;
    }
    let exit = serde_json::json!({
        "type": "exit",
        "code": outcome.exit_code,
        "signal": outcome.signal,
        "duration_ms": outcome.duration_ms,
        "timed_out": outcome.timed_out,
    });
    writeln!(out, "{}", exit)?;
    Ok(())
}

/// Split `s` into <=`n`-byte chunks at char boundaries.  64 KiB is
/// spec memo 11 §3.4's "no frame larger than 64 KiB" limit; we use
/// 60_000 to leave headroom for the JSON wrapping.
fn chunked(s: &str, n: usize) -> Vec<String> {
    if s.len() <= n {
        return vec![s.to_string()];
    }
    let mut out = Vec::new();
    let mut start = 0;
    while start < s.len() {
        let mut end = (start + n).min(s.len());
        while !s.is_char_boundary(end) && end > start {
            end -= 1;
        }
        out.push(s[start..end].to_string());
        start = end;
    }
    out
}

pub fn cmd_exec(args: ExecArgs, paths: &crate::paths::Paths, quiet: bool) -> Result<()> {
    let socket = pool_client::pool_socket_path(&paths.runtime_dir, &args.name);
    if !socket.exists() {
        bail!(
            "no pool daemon at {} (start one with `umlctl pool serve --name {}`)",
            socket.display(),
            args.name
        );
    }

    let req = build_exec_request(&args)?;
    let started_ns = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let t0 = Instant::now();

    let reply = pool_client::rpc(&socket, &req).context("exec RPC")?;
    let mut outcome = decode_exec_reply(&reply)?;
    // If the daemon doesn't supply a duration, fall back to ours.
    if outcome.duration_ms == 0 {
        outcome.duration_ms = t0.elapsed().as_millis() as u64;
    }

    if args.json {
        let stdout = std::io::stdout();
        let mut lock = stdout.lock();
        emit_ndjson_frames(&mut lock, &args, &outcome, started_ns)?;
    } else {
        // Transparent pass-through for interactive operators.
        let stdout = std::io::stdout();
        let stderr = std::io::stderr();
        {
            let mut lock = stdout.lock();
            lock.write_all(outcome.stdout.as_bytes()).ok();
        }
        {
            let mut lock = stderr.lock();
            lock.write_all(outcome.stderr.as_bytes()).ok();
        }
        if let Some(err) = &outcome.error {
            if !quiet {
                eprintln!("umlctl exec: daemon error: {}", err);
            }
        }
    }

    let rc = if outcome.timed_out {
        124
    } else if outcome.signal != 0 {
        128 + outcome.signal
    } else {
        outcome.exit_code
    };
    if rc != 0 {
        std::process::exit(rc);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(argv: &[&str]) -> ExecArgs {
        ExecArgs {
            name: "default".to_string(),
            pid: 1234,
            timeout: 0,
            json: true,
            cwd: None,
            envs: vec![],
            argv: argv.iter().map(|s| s.to_string()).collect(),
        }
    }

    #[test]
    fn build_request_round_trip() {
        let mut a = args(&["/bin/sh", "-c", "echo hi"]);
        a.envs.push("FOO=bar".to_string());
        a.cwd = Some("/tmp".to_string());
        a.timeout = 30;
        let req = build_exec_request(&a).unwrap();
        assert_eq!(req["op"], "exec");
        assert_eq!(req["pid"], 1234);
        assert_eq!(req["timeout_secs"], 30);
        assert_eq!(req["cwd"], "/tmp");
        assert_eq!(req["argv"][0], "/bin/sh");
        assert_eq!(req["argv"][2], "echo hi");
        assert_eq!(req["env"]["FOO"], "bar");
    }

    #[test]
    fn build_request_rejects_empty_argv() {
        let a = args(&[]);
        assert!(build_exec_request(&a).is_err());
    }

    #[test]
    fn build_request_rejects_bare_env() {
        let mut a = args(&["true"]);
        a.envs.push("OOPS".to_string());
        assert!(build_exec_request(&a).is_err());
    }

    #[test]
    fn decode_ok_reply() {
        let r = serde_json::json!({
            "ok": true,
            "stdout": "hello\n",
            "stderr": "",
            "exit": 0,
            "signal": 0,
            "duration_ms": 12,
        });
        let o = decode_exec_reply(&r).unwrap();
        assert_eq!(o.stdout, "hello\n");
        assert_eq!(o.exit_code, 0);
        assert_eq!(o.duration_ms, 12);
        assert!(o.error.is_none());
    }

    #[test]
    fn decode_error_reply_becomes_exit_one() {
        let r = serde_json::json!({"ok": false, "error": "no such pid"});
        let o = decode_exec_reply(&r).unwrap();
        assert_eq!(o.exit_code, 1);
        assert_eq!(o.error.as_deref(), Some("no such pid"));
    }

    #[test]
    fn decode_missing_fields_default_to_empty() {
        let r = serde_json::json!({"ok": true});
        let o = decode_exec_reply(&r).unwrap();
        assert_eq!(o.exit_code, 0);
        assert_eq!(o.stdout, "");
        assert_eq!(o.stderr, "");
    }

    #[test]
    fn ndjson_frames_order_start_data_exit() {
        let a = args(&["/bin/echo", "hi"]);
        let outcome = ExecOutcome {
            stdout: "hi\n".to_string(),
            stderr: String::new(),
            console: String::new(),
            exit_code: 0,
            signal: 0,
            timed_out: false,
            duration_ms: 5,
            error: None,
        };
        let mut buf = Vec::new();
        emit_ndjson_frames(&mut buf, &a, &outcome, 123).unwrap();
        let s = String::from_utf8(buf).unwrap();
        let lines: Vec<&str> = s.lines().collect();
        assert!(lines[0].contains("\"type\":\"start\""), "got: {}", lines[0]);
        assert!(lines[0].contains("\"schema_version\":\"exec/1\""));
        assert!(lines[0].contains("\"pid\":1234"));
        assert!(lines[1].contains("\"type\":\"stdout\""));
        assert!(lines[1].contains("hi"));
        assert!(lines[2].contains("\"type\":\"exit\""));
        assert!(lines[2].contains("\"code\":0"));
        assert!(lines[2].contains("\"timed_out\":false"));
    }

    #[test]
    fn ndjson_includes_console_when_present() {
        let a = args(&["true"]);
        let outcome = ExecOutcome {
            stdout: String::new(),
            stderr: String::new(),
            console: "[oops]".to_string(),
            exit_code: 0,
            signal: 0,
            timed_out: false,
            duration_ms: 1,
            error: None,
        };
        let mut buf = Vec::new();
        emit_ndjson_frames(&mut buf, &a, &outcome, 0).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("\"type\":\"console\""));
        assert!(s.contains("[oops]"));
    }

    #[test]
    fn ndjson_surfaces_daemon_error_as_stderr_frame() {
        let a = args(&["true"]);
        let outcome = ExecOutcome {
            stdout: String::new(),
            stderr: String::new(),
            console: String::new(),
            exit_code: 1,
            signal: 0,
            timed_out: false,
            duration_ms: 0,
            error: Some("no such pid".to_string()),
        };
        let mut buf = Vec::new();
        emit_ndjson_frames(&mut buf, &a, &outcome, 0).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("\"type\":\"stderr\""));
        assert!(s.contains("no such pid"));
    }

    #[test]
    fn chunked_splits_large_payload() {
        let big = "x".repeat(130_000);
        let parts = chunked(&big, 60_000);
        assert!(parts.len() >= 3);
        assert_eq!(parts.iter().map(|p| p.len()).sum::<usize>(), big.len());
        for p in &parts {
            assert!(p.len() <= 60_000);
        }
    }
}
