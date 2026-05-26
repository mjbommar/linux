// SPDX-License-Identifier: GPL-2.0
//
// umlctl pool serve — long-lived fork-server supervisor (Memo 09
// Phase 1c).
//
// Boots ONE master kernel built with CONFIG_UM_TEMPLATE_PAUSE_FORK=y
// in `um_template_pause=fork` mode, then listens on a Unix-domain
// socket at `$XDG_RUNTIME_DIR/uml/pools/<name>/api.sock` for line-
// delimited JSON RPCs.  Each `take` rewrites the identity memfd,
// SIGCONTs the master, waits for it to re-SIGSTOP, and reads the
// new child host pid back from memfd[260:264].
//
// Wire format (one JSON object per line, both directions):
//
//   --> {"op":"take","instance":"…","mac":"…","tap":"…",
//        "ipv4":"…","gateway":"…","mconsole":""}
//   <-- {"ok":true,"result":<SpawnResult>}
//
//   --> {"op":"list"}
//   <-- {"ok":true,"members":[<SpawnResult>, …]}
//
//   --> {"op":"status"}
//   <-- {"ok":true,"master_pid":N,"taken":K,"socket":"…","name":"…"}
//
//   --> {"op":"destroy","pid":N}
//   <-- {"ok":true,"destroyed":true,"pid":N}
//
//   --> {"op":"shutdown"}
//   <-- {"ok":true}
//
// On any error: {"ok":false,"error":"<message>"}.
//
// The `take` response envelope is exactly the `SpawnResult` shape
// `umlctl pool spawn` emits when --json is given (Phase 1b), so any
// existing caller can be repointed at the daemon without code
// changes.
//
// Concurrency: a single thread serializes all take operations
// because the identity memfd (write blob at offset 0 / read pid at
// offset 260) and the SIGCONT/re-SIGSTOP handshake are not safely
// pipelinable.  Accept loop uses a poll() with the listen-fd plus
// a self-pipe so SIGTERM/SIGINT/SIGCHLD wake the loop deterministic-
// ally.  This keeps the daemon under ~400 LoC and easy to audit.

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::os::unix::net::{UnixListener, UnixStream};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::pool;

#[derive(Args, Debug)]
pub struct ServeArgs {
    /// Pool name; determines the socket path
    /// `$XDG_RUNTIME_DIR/uml/pools/<name>/api.sock`.
    #[arg(long, default_value = "default", value_name = "NAME")]
    pub name: String,

    /// Path to the UML kernel binary.  Must be built with
    /// CONFIG_UM_TEMPLATE_PAUSE_FORK=y.  If absent, --fork-kernel
    /// (or $UM_FORK_KERNEL) is consulted; this matches `umlctl
    /// mission`'s Phase 8 resolution discipline.
    #[arg(long, value_name = "PATH")]
    pub kernel: Option<PathBuf>,

    /// Alias for --kernel; the daemon requires a fork-capable build.
    #[arg(long, env = "UM_FORK_KERNEL", value_name = "PATH")]
    pub fork_kernel: Option<PathBuf>,

    /// mem= argument passed on the cmdline.  Default 128M.
    #[arg(long, default_value = "128M", value_name = "SIZE")]
    pub mem: String,

    /// Extra cmdline appended after the template-pause-mandatory
    /// arguments.
    #[arg(long, value_name = "STRING", default_value = "")]
    pub cmdline: String,

    /// rootfs args for the guest cmdline.
    #[arg(
        long,
        default_value = "rootfstype=hostfs rootflags=/ root=/dev/root rw",
        value_name = "STRING"
    )]
    pub rootfs: String,

    /// Optional path to an init script.  If omitted, a default
    /// pool-member init (mounts /proc, writes to
    /// /proc/um/template_pause, sleeps) is materialized.
    #[arg(long, value_name = "PATH")]
    pub init: Option<PathBuf>,

    /// Minimum number of warm pre-forked members the daemon should
    /// try to maintain.  MVP: option wired through, replenish loop
    /// is a TODO (see `replenish_warm_pool`).  0 means lazy-only.
    #[arg(long, default_value_t = 0, value_name = "N")]
    pub min_warm: u32,

    /// Detach from the controlling terminal (fork+setsid) before
    /// entering the accept loop.  Pidfile is written to
    /// $XDG_RUNTIME_DIR/uml/pools/<name>/serve.pid.
    #[arg(long)]
    pub background: bool,
}

/// One entry in the daemon's in-memory member table.
#[derive(Clone, serde::Serialize, serde::Deserialize)]
struct MemberRecord {
    pid: i32,
    instance: String,
    mac: String,
    tap: String,
    ipv4_cidr: String,
    ipv4_gateway: String,
    mconsole_path: String,
}

/// Wire-level RPC request.  Tagged on `op`.
#[derive(serde::Deserialize, Debug, PartialEq)]
#[serde(tag = "op", rename_all = "lowercase")]
pub(crate) enum Request {
    Take {
        #[serde(default)]
        instance: String,
        #[serde(default)]
        mac: String,
        #[serde(default)]
        tap: String,
        #[serde(default)]
        ipv4: String,
        #[serde(default)]
        gateway: String,
        #[serde(default)]
        mconsole: String,
    },
    List,
    Status,
    Destroy {
        pid: i32,
    },
    Exec {
        pid: i32,
        argv: Vec<String>,
        #[serde(default)]
        env: HashMap<String, String>,
        #[serde(default)]
        cwd: String,
        #[serde(default)]
        timeout_secs: u64,
    },
    Shutdown,
}

/// Parse one JSON line.  Lenient with surrounding whitespace.
pub(crate) fn parse_request(line: &str) -> Result<Request> {
    let trimmed = line.trim();
    if trimmed.is_empty() {
        bail!("empty request line");
    }
    serde_json::from_str(trimmed).with_context(|| format!("parse RPC request {:?}", trimmed))
}

/// Where the per-pool runtime directory lives.
fn pool_dir(runtime_dir: &Path, name: &str) -> PathBuf {
    runtime_dir.join("pools").join(name)
}

fn pool_socket(runtime_dir: &Path, name: &str) -> PathBuf {
    pool_dir(runtime_dir, name).join("api.sock")
}

fn pool_pidfile(runtime_dir: &Path, name: &str) -> PathBuf {
    pool_dir(runtime_dir, name).join("serve.pid")
}

/// Daemon state, owned by the single event-loop thread.
struct DaemonState {
    name: String,
    socket_path: PathBuf,
    pidfile: PathBuf,
    master_pid: i32,
    identity_memfd: OwnedFd,
    members: Mutex<HashMap<i32, MemberRecord>>,
    shutdown_requested: AtomicI32,
    min_warm: u32,
}

impl DaemonState {
    fn handle(&self, req: Request) -> serde_json::Value {
        match req {
            Request::Take {
                instance,
                mac,
                tap,
                ipv4,
                gateway,
                mconsole,
            } => match self.do_take(&instance, &mac, &tap, &ipv4, &gateway, &mconsole) {
                Ok(rec) => serde_json::json!({"ok": true, "result": rec}),
                Err(e) => serde_json::json!({"ok": false, "error": format!("{:#}", e)}),
            },
            Request::List => {
                self.reap_dead();
                let members = self.members.lock().unwrap();
                let v: Vec<&MemberRecord> = members.values().collect();
                serde_json::json!({"ok": true, "members": v})
            }
            Request::Status => {
                self.reap_dead();
                let members = self.members.lock().unwrap();
                serde_json::json!({
                    "ok": true,
                    "name": self.name,
                    "master_pid": self.master_pid,
                    "taken": members.len(),
                    "socket": self.socket_path.display().to_string(),
                    "min_warm": self.min_warm,
                })
            }
            Request::Destroy { pid } => match self.do_destroy(pid) {
                Ok(destroyed) => {
                    serde_json::json!({"ok": true, "destroyed": destroyed, "pid": pid})
                }
                Err(e) => serde_json::json!({"ok": false, "error": format!("{:#}", e)}),
            },
            Request::Exec {
                pid,
                argv,
                env,
                cwd,
                timeout_secs,
            } => match self.do_exec(pid, &argv, &env, &cwd, timeout_secs) {
                Ok(v) => v,
                Err(e) => serde_json::json!({"ok": false, "error": format!("{:#}", e)}),
            },
            Request::Shutdown => {
                self.shutdown_requested.store(1, Ordering::SeqCst);
                serde_json::json!({"ok": true})
            }
        }
    }

    /// Drive one fork-on-resume cycle and capture the resulting
    /// child pid.  Holds the implicit single-threaded lock via the
    /// outer event loop; do not call concurrently.
    /// Per spec memo 11 §6 question 2: synthesize a per-instance
    /// mconsole socket path when the caller leaves it empty.  This
    /// keeps `umlctl exec` working even when the take RPC came from a
    /// caller (eg the syzkaller shim) that doesn't care to pick the
    /// path.  The directory is the per-pool runtime dir, which the
    /// daemon already creates in `cmd_serve`.
    fn synthesize_mconsole_path(&self, instance: &str) -> String {
        let dir = self
            .socket_path
            .parent()
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| PathBuf::from("/tmp"));
        let safe: String = instance
            .chars()
            .map(|c| {
                if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                    c
                } else {
                    '_'
                }
            })
            .collect();
        dir.join(format!("{}.mconsole", safe)).display().to_string()
    }

    fn do_take(
        &self,
        instance: &str,
        mac: &str,
        tap: &str,
        ipv4: &str,
        gateway: &str,
        mconsole: &str,
    ) -> Result<MemberRecord> {
        let synth_storage;
        let mconsole = if mconsole.is_empty() {
            synth_storage = self.synthesize_mconsole_path(instance);
            synth_storage.as_str()
        } else {
            mconsole
        };
        let mac_bytes = pool::parse_mac(mac).context("parse mac")?;
        let blob = pool::build_identity_blob(instance, &mac_bytes, tap, ipv4, gateway, mconsole)
            .context("build identity blob")?;

        // Rewrite the identity blob in the memfd at offset 0.
        let fd = self.identity_memfd.as_raw_fd();
        let n = unsafe { libc::pwrite(fd, blob.as_ptr() as *const _, blob.len(), 0) };
        if n != blob.len() as isize {
            return Err(std::io::Error::last_os_error()).context("pwrite identity blob into memfd");
        }
        // Zero the child-pid slot before the take so we can detect a
        // failed write-back unambiguously.
        let zero: u32 = 0;
        let z = unsafe {
            libc::pwrite(
                fd,
                &zero as *const _ as *const _,
                std::mem::size_of::<u32>(),
                pool::IDENTITY_CHILD_PID_OFFSET as libc::off_t,
            )
        };
        if z != std::mem::size_of::<u32>() as isize {
            return Err(std::io::Error::last_os_error()).context("pwrite zero child-pid slot");
        }

        // SIGCONT the master; it will fork, kill the M-fork child,
        // write the new child's pid into the memfd, then re-SIGSTOP.
        let r = unsafe { libc::kill(self.master_pid, libc::SIGCONT) };
        if r < 0 {
            return Err(std::io::Error::last_os_error()).context("SIGCONT master");
        }

        // Wait for the master to re-pause.  We poll via WUNTRACED
        // + WNOHANG with a deadline; the kernel reaches the pause
        // within ~50ms on a healthy fork build.  Generous timeout so
        // an overloaded host still completes the take.
        wait_for_stop(self.master_pid, Duration::from_secs(10))
            .context("wait for master to re-SIGSTOP after take")?;

        // Read the child pid the master wrote at offset 260.
        let mut buf = [0u8; 4];
        let r = unsafe {
            libc::pread(
                fd,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                pool::IDENTITY_CHILD_PID_OFFSET as libc::off_t,
            )
        };
        if r != buf.len() as isize {
            return Err(std::io::Error::last_os_error()).context("pread child pid from memfd");
        }
        let child_pid = u32::from_le_bytes(buf) as i32;
        if child_pid <= 0 {
            bail!("master did not report child pid (memfd slot still zero)");
        }

        let rec = MemberRecord {
            pid: child_pid,
            instance: instance.to_string(),
            mac: mac.to_string(),
            tap: tap.to_string(),
            ipv4_cidr: ipv4.to_string(),
            ipv4_gateway: gateway.to_string(),
            mconsole_path: mconsole.to_string(),
        };
        self.members.lock().unwrap().insert(child_pid, rec.clone());
        Ok(rec)
    }

    /// `exec` RPC handler.  Spec memo 11 §3.1.  Today's MVP backend
    /// drives the member's mconsole socket via the standard
    /// `uml_mconsole(1)` tool if it's installed; otherwise the verb
    /// returns a clean diagnostic instead of pretending to work.
    ///
    /// Returns a JSON object shaped as the spec's exec reply
    /// envelope: `{"ok":true, "stdout":"…", "stderr":"…", "exit":N,
    /// "signal":S, "duration_ms":D, "timed_out":bool}` on completion,
    /// or `{"ok":false, "error":"…"}` on failure.
    ///
    /// Note on the in-guest exec primitive: `uml_mconsole exec` is
    /// the standard Linux upstream tool but is not present on all
    /// hosts; when it's absent we surface that as a clear operator
    /// message rather than silently producing empty output.  Future
    /// work (spec memo 11 §6 question 3) replaces this with a pty
    /// pair + SCM_RIGHTS-passed master fd; the envelope stays the
    /// same so callers don't need to change.
    fn do_exec(
        &self,
        pid: i32,
        argv: &[String],
        env: &HashMap<String, String>,
        cwd: &str,
        timeout_secs: u64,
    ) -> Result<serde_json::Value> {
        if argv.is_empty() {
            bail!("exec requires non-empty argv");
        }
        let member = {
            let members = self.members.lock().unwrap();
            members
                .get(&pid)
                .cloned()
                .ok_or_else(|| anyhow!("no such pool member pid {}", pid))?
        };
        if member.mconsole_path.is_empty() {
            bail!(
                "pool member pid {} has no mconsole socket; respawn the daemon \
                 against a kernel that exposes one",
                pid
            );
        }
        if !Path::new(&member.mconsole_path).exists() {
            bail!(
                "pool member pid {} mconsole socket {:?} not present yet \
                 (the in-guest exec primitive is not available in this build; \
                 see Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-next-sprint/11-syzkaller-shim-spec.md §6 q2)",
                pid, member.mconsole_path
            );
        }
        // We expose the standard `uml_mconsole exec` shape here.  If
        // the host doesn't have it, surface that explicitly.
        let tool = which_uml_mconsole();
        let tool = tool.ok_or_else(|| {
            anyhow!(
                "uml_mconsole(1) not found in PATH; install the `user-mode-linux-tools` \
                 package or set UML_MCONSOLE=/path/to/uml_mconsole"
            )
        })?;
        let mut cmd_str = String::from("exec ");
        // mconsole exec expects a single shell line; quote-safe-join.
        for (i, a) in argv.iter().enumerate() {
            if i > 0 {
                cmd_str.push(' ');
            }
            cmd_str.push_str(&shell_quote(a));
        }
        if !cwd.is_empty() {
            cmd_str = format!("cd {} && {}", shell_quote(cwd), cmd_str);
        }
        for (k, v) in env {
            cmd_str = format!("{}={} {}", shell_quote(k), shell_quote(v), cmd_str);
        }

        let started = Instant::now();
        let mut child = Command::new(&tool)
            .arg(&member.mconsole_path)
            .arg(&cmd_str)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .with_context(|| format!("spawn {}", tool.display()))?;

        let deadline = if timeout_secs > 0 {
            Some(Instant::now() + Duration::from_secs(timeout_secs))
        } else {
            None
        };
        let mut timed_out = false;
        loop {
            match child.try_wait() {
                Ok(Some(_)) => break,
                Ok(None) => {
                    if let Some(d) = deadline {
                        if Instant::now() >= d {
                            let _ = child.kill();
                            timed_out = true;
                            break;
                        }
                    }
                    std::thread::sleep(Duration::from_millis(20));
                }
                Err(e) => return Err(anyhow!("wait for mconsole exec: {}", e)),
            }
        }
        let output = child
            .wait_with_output()
            .context("collect mconsole exec output")?;
        let duration_ms = started.elapsed().as_millis() as u64;
        let exit_code = output
            .status
            .code()
            .unwrap_or(if timed_out { 124 } else { -1 });
        let signal = output.status.signal_from_status().unwrap_or(0);

        Ok(serde_json::json!({
            "ok": true,
            "stdout": String::from_utf8_lossy(&output.stdout).to_string(),
            "stderr": String::from_utf8_lossy(&output.stderr).to_string(),
            "exit": exit_code,
            "signal": signal,
            "duration_ms": duration_ms,
            "timed_out": timed_out,
        }))
    }

    fn do_destroy(&self, pid: i32) -> Result<bool> {
        let _removed = self.members.lock().unwrap().remove(&pid).is_some();
        // "Destroyed" semantics: the member is no longer a runnable
        // host process.  This includes Z (zombie) — the master kills
        // the M-fork child immediately post-fork, so by the time the
        // caller asks us to destroy it the pid is typically already a
        // zombie waiting for the master to wait4() it.  We treat
        // zombies and gone-from-/proc identically: not-runnable.
        if !pid_runnable(pid) {
            return Ok(true);
        }
        let r = unsafe { libc::kill(pid, libc::SIGKILL) };
        if r < 0 {
            // ESRCH is fine — the pid vanished between the check and
            // the kill.  Anything else is a real failure.
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::ESRCH) {
                return Ok(true);
            }
            return Err(err).context("SIGKILL pool member");
        }
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline {
            if !pid_runnable(pid) {
                return Ok(true);
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        Ok(!pid_runnable(pid))
    }

    /// Reap zombies + prune dead entries from the member table.
    /// Called on every observable event (RPC arrival or SIGCHLD).
    fn reap_dead(&self) {
        // Drain any pending zombies.  WNOHANG so we never block.
        loop {
            let mut status: libc::c_int = 0;
            let r = unsafe { libc::waitpid(-1, &mut status as *mut _, libc::WNOHANG) };
            if r <= 0 {
                break;
            }
        }
        let mut members = self.members.lock().unwrap();
        members.retain(|pid, _| pool::pid_is_alive(*pid));
    }
}

/// True iff `pid` exists in /proc AND is NOT a zombie (Z) or about-to-
/// be-reaped (X) state.  Used by the destroy path because the master's
/// post-fork SIGKILL of the M-fork child often leaves the child as a
/// zombie owned by the master — the daemon cannot wait4 it directly,
/// but it should still report "destroyed" to the caller.
fn pid_runnable(pid: i32) -> bool {
    let stat = match std::fs::read_to_string(format!("/proc/{}/stat", pid)) {
        Ok(s) => s,
        Err(_) => return false,
    };
    let Some(close) = stat.rfind(')') else {
        return false;
    };
    let rest = &stat[close + 1..];
    let state = rest.trim_start().chars().next().unwrap_or('?');
    !matches!(state, 'Z' | 'X')
}

/// Wait for `pid` to enter T (stopped) state, polling /proc/<pid>/stat.
/// We use /proc rather than waitpid(WUNTRACED) because the caller is
/// the kernel via fork-on-resume — the master keeps sending SIGSTOP
/// to itself, and a parent waitpid only collects ONE stop-notification
/// per iteration.  /proc shows the cumulative state.
///
/// Poll cadence is tuned to Memo 09 Phase 3's `take.p50 ≤ 5 ms` gate:
/// the fast path (master re-pauses in <2 ms in steady state) needs a
/// sub-ms quantum or the gate's measured latency tail is dominated by
/// sleep, not by kernel work.  Adaptive backoff starts at 250 µs (fast
/// path; ~4 polls cover the typical kernel fork + identity-apply +
/// SIGSTOP window), then doubles every 8 iterations to 1 ms, 4 ms, and
/// finally 16 ms.  The slow path (cold-boot, pool replenish) reaches
/// the 16 ms cap after ~32 iterations (~75 ms of busy-poll equivalent),
/// matching the prior 10 ms-throughout cost.
fn wait_for_stop(pid: i32, deadline: Duration) -> Result<()> {
    let end = Instant::now() + deadline;
    let stat_path = format!("/proc/{}/stat", pid);
    let mut iter: u32 = 0;
    while Instant::now() < end {
        match std::fs::read_to_string(&stat_path) {
            Ok(s) => {
                // /proc/<pid>/stat field 3 is process state.  Parse
                // safely around the comm field which can contain
                // spaces and parens.
                if let Some(close) = s.rfind(')') {
                    let rest = &s[close + 1..];
                    let state = rest.trim_start().chars().next();
                    match state {
                        Some('T') | Some('t') => return Ok(()),
                        Some('Z') | Some('X') => {
                            bail!("master pid {} died (state={:?})", pid, state)
                        }
                        _ => {}
                    }
                }
            }
            Err(e) => bail!("read {}: {}", stat_path, e),
        }
        let sleep_us: u64 = match iter / 8 {
            0 => 250,    // 0..7  → 250 µs (sub-ms detection)
            1 => 1_000,  // 8..15 → 1 ms
            2 => 4_000,  // 16..23 → 4 ms
            _ => 16_000, // 24..  → 16 ms cap
        };
        std::thread::sleep(Duration::from_micros(sleep_us));
        iter = iter.saturating_add(1);
    }
    bail!("timeout waiting for pid {} to re-SIGSTOP", pid)
}

/// Build + launch the master process and wait for its first SIGSTOP.
fn launch_master(args: &ServeArgs) -> Result<(Child, OwnedFd)> {
    let kernel = args
        .kernel
        .clone()
        .or_else(|| args.fork_kernel.clone())
        .ok_or_else(|| anyhow!("--kernel or --fork-kernel is required"))?;
    if !kernel.exists() {
        bail!("kernel not found: {}", kernel.display());
    }
    let kernel = std::fs::canonicalize(&kernel)
        .with_context(|| format!("canonicalize {}", kernel.display()))?;

    // Pre-fill with the first member's identity-blob template (all
    // empty strings) — `take` rewrites at offset 0 each cycle.
    let blob = pool::build_identity_blob("pool-master", &[0u8; 6], "", "", "", "")?;
    let memfd = pool::create_identity_memfd(&blob).context("create identity memfd")?;
    let memfd_raw = memfd.as_raw_fd();

    let init_path = match &args.init {
        Some(p) => p.clone(),
        None => pool::default_init_script("pool-master")?,
    };

    let mut argv: Vec<String> = vec![
        kernel.display().to_string(),
        format!("mem={}", args.mem),
        "um_template_pause=fork".to_string(),
    ];
    for tok in args.rootfs.split_whitespace() {
        argv.push(tok.to_string());
    }
    argv.push(format!("init={}", init_path.display()));
    for tok in args.cmdline.split_whitespace() {
        argv.push(tok.to_string());
    }

    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]);
    cmd.env("UM_TEMPLATE_IDENTITY_FD", memfd_raw.to_string());
    let memfd_for_preexec = memfd_raw;
    unsafe {
        cmd.pre_exec(move || {
            let flags = libc::fcntl(memfd_for_preexec, libc::F_GETFD);
            if flags < 0 {
                return Err(std::io::Error::last_os_error());
            }
            if libc::fcntl(memfd_for_preexec, libc::F_SETFD, flags & !libc::FD_CLOEXEC) < 0 {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    cmd.stdout(std::process::Stdio::null());
    cmd.stderr(std::process::Stdio::null());

    let child = cmd.spawn().with_context(|| {
        format!(
            "spawn master {} (is CONFIG_UM_TEMPLATE_PAUSE_FORK=y?)",
            argv[0]
        )
    })?;
    let pid = child.id() as i32;
    wait_for_stop(pid, Duration::from_secs(60))
        .with_context(|| format!("wait for master pid {} to SIGSTOP", pid))?;
    Ok((child, memfd))
}

/// Wake the event loop on signals by writing to a self-pipe.
/// Returns (read_end, write_end) — read is poll()'d; write is the
/// signal-handler's drop point.
fn make_self_pipe() -> Result<(OwnedFd, OwnedFd)> {
    let mut fds = [0i32; 2];
    let r = unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC | libc::O_NONBLOCK) };
    if r < 0 {
        return Err(std::io::Error::last_os_error()).context("pipe2 for self-pipe");
    }
    use std::os::fd::FromRawFd;
    let r = unsafe { OwnedFd::from_raw_fd(fds[0]) };
    let w = unsafe { OwnedFd::from_raw_fd(fds[1]) };
    Ok((r, w))
}

static SELF_PIPE_WRITE: AtomicI32 = AtomicI32::new(-1);

extern "C" fn signal_thunk(_sig: libc::c_int) {
    let fd = SELF_PIPE_WRITE.load(Ordering::Relaxed);
    if fd >= 0 {
        let byte: u8 = 1;
        unsafe {
            libc::write(fd, &byte as *const _ as *const _, 1);
        }
    }
}

fn install_signal_handlers(write_fd: RawFd) -> Result<()> {
    SELF_PIPE_WRITE.store(write_fd, Ordering::Relaxed);
    let mut sa: libc::sigaction = unsafe { std::mem::zeroed() };
    sa.sa_sigaction = signal_thunk as *const () as usize;
    sa.sa_flags = libc::SA_RESTART;
    for sig in &[libc::SIGINT, libc::SIGTERM, libc::SIGCHLD] {
        let r = unsafe { libc::sigaction(*sig, &sa, std::ptr::null_mut()) };
        if r < 0 {
            return Err(std::io::Error::last_os_error()).context("sigaction");
        }
    }
    Ok(())
}

/// Daemonize: fork, exit the parent, setsid in the child.  Returns
/// in the child after detaching from the controlling terminal.
fn daemonize(pidfile: &Path) -> Result<()> {
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        return Err(std::io::Error::last_os_error()).context("fork for --background");
    }
    if pid > 0 {
        // Parent: write the child's pid to the pidfile, then exit.
        std::fs::write(pidfile, format!("{}\n", pid))
            .with_context(|| format!("write pidfile {}", pidfile.display()))?;
        unsafe { libc::_exit(0) };
    }
    let r = unsafe { libc::setsid() };
    if r < 0 {
        return Err(std::io::Error::last_os_error()).context("setsid");
    }
    // Redirect stdio to /dev/null so the daemon can't surprise-write.
    // Open via std::fs::File so we don't need a libc::open + raw C-string;
    // the resulting fd is dup2'd into 0/1/2 and the File is dropped.
    let devnull = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open("/dev/null")
        .ok();
    if let Some(f) = &devnull {
        let raw = f.as_raw_fd();
        for fd in 0..3 {
            unsafe { libc::dup2(raw, fd) };
        }
    }
    drop(devnull);
    Ok(())
}

pub fn cmd_serve(args: ServeArgs, paths: &crate::paths::Paths, _quiet: bool) -> Result<()> {
    let dir = pool_dir(&paths.runtime_dir, &args.name);
    std::fs::create_dir_all(&dir).with_context(|| format!("create {}", dir.display()))?;
    let socket_path = pool_socket(&paths.runtime_dir, &args.name);
    let pidfile = pool_pidfile(&paths.runtime_dir, &args.name);

    if socket_path.exists() {
        // Stale socket from a prior daemon.  We could probe-and-fail
        // if another daemon is live, but Phase 1c-MVP keeps it simple:
        // unlink and rebind.  A second concurrent daemon is operator
        // error; we don't try to fence it.
        std::fs::remove_file(&socket_path).ok();
    }

    if args.background {
        daemonize(&pidfile).context("daemonize")?;
    } else {
        // Foreground: still write the pidfile so siblings can find us.
        std::fs::write(&pidfile, format!("{}\n", std::process::id()))
            .with_context(|| format!("write pidfile {}", pidfile.display()))?;
    }

    let (master_child, identity_memfd) = launch_master(&args).context("launch master")?;
    let master_pid = master_child.id() as i32;
    // Avoid std::process::Child reaping the master on Drop — we
    // manage its lifecycle explicitly through the event loop.
    std::mem::forget(master_child);

    let listener = UnixListener::bind(&socket_path)
        .with_context(|| format!("bind {}", socket_path.display()))?;
    listener
        .set_nonblocking(true)
        .context("set listener non-blocking")?;

    let (read_pipe, write_pipe) = make_self_pipe()?;
    install_signal_handlers(write_pipe.as_raw_fd()).context("install signal handlers")?;

    let state = DaemonState {
        name: args.name.clone(),
        socket_path: socket_path.clone(),
        pidfile: pidfile.clone(),
        master_pid,
        identity_memfd,
        members: Mutex::new(HashMap::new()),
        shutdown_requested: AtomicI32::new(0),
        min_warm: args.min_warm,
    };

    // Optional pre-warm: out of MVP scope, but stub the hook in place.
    if state.min_warm > 0 {
        eprintln!(
            "umlctl pool serve: min_warm={} requested; replenish loop is a TODO",
            state.min_warm
        );
    }

    let result = run_accept_loop(&state, &listener, &read_pipe);

    // Graceful shutdown: kill all children + master, then unlink.
    {
        let members = state.members.lock().unwrap();
        for pid in members.keys() {
            unsafe { libc::kill(*pid, libc::SIGKILL) };
        }
    }
    unsafe { libc::kill(state.master_pid, libc::SIGKILL) };
    let _ = std::fs::remove_file(&state.socket_path);
    let _ = std::fs::remove_file(&state.pidfile);
    let _ = std::fs::remove_dir(&dir);

    // Final reap pass.
    let deadline = Instant::now() + Duration::from_secs(3);
    while Instant::now() < deadline {
        let r = unsafe { libc::waitpid(-1, std::ptr::null_mut(), libc::WNOHANG) };
        if r <= 0 {
            break;
        }
    }

    result
}

fn run_accept_loop(
    state: &DaemonState,
    listener: &UnixListener,
    read_pipe: &OwnedFd,
) -> Result<()> {
    let lfd = listener.as_raw_fd();
    let pfd = read_pipe.as_raw_fd();
    loop {
        if state.shutdown_requested.load(Ordering::SeqCst) != 0 {
            return Ok(());
        }
        let mut fds = [
            libc::pollfd {
                fd: lfd,
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: pfd,
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        let r = unsafe { libc::poll(fds.as_mut_ptr(), 2, 1000) };
        if r < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            return Err(err).context("poll(2)");
        }
        // Always drain the self-pipe and reap zombies.  This catches
        // SIGCHLD races where the signal landed between polls.
        if fds[1].revents & libc::POLLIN != 0 {
            let mut sink = [0u8; 64];
            unsafe {
                libc::read(pfd, sink.as_mut_ptr() as *mut _, sink.len());
            }
        }
        state.reap_dead();
        // If the master died unexpectedly, give up — restarting it is
        // out of scope for the MVP.
        if !pool::pid_is_alive(state.master_pid) {
            return Err(anyhow!("master pid {} died unexpectedly", state.master_pid));
        }
        if fds[0].revents & libc::POLLIN != 0 {
            match listener.accept() {
                Ok((stream, _)) => {
                    handle_client(state, stream);
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(e) => {
                    eprintln!("umlctl pool serve: accept: {}", e);
                }
            }
        }
    }
}

fn handle_client(state: &DaemonState, stream: UnixStream) {
    // One request per connection keeps the protocol trivial.  A
    // client that wants to batch can reconnect; the cost is one
    // socket(2)+connect(2) per RPC, ~10us locally.
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(5)));
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    match reader.read_line(&mut line) {
        Ok(0) => return,
        Ok(_) => {}
        Err(e) => {
            eprintln!("umlctl pool serve: read: {}", e);
            return;
        }
    }
    let resp = match parse_request(&line) {
        Ok(req) => state.handle(req),
        Err(e) => serde_json::json!({"ok": false, "error": format!("{:#}", e)}),
    };
    let mut stream = reader.into_inner();
    let serialized = match serde_json::to_string(&resp) {
        Ok(s) => s,
        Err(e) => format!("{{\"ok\":false,\"error\":\"serialize: {}\"}}", e),
    };
    if let Err(e) = writeln!(stream, "{}", serialized) {
        eprintln!("umlctl pool serve: write: {}", e);
    }
}

/// Locate `uml_mconsole(1)` on the host.  Prefers $UML_MCONSOLE,
/// then PATH lookup.  Returns None if absent so callers can produce a
/// user-friendly error rather than letting `Command::spawn` fail with
/// ENOENT.
fn which_uml_mconsole() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("UML_MCONSOLE") {
        let pb = PathBuf::from(&p);
        if pb.exists() {
            return Some(pb);
        }
    }
    let path = std::env::var("PATH").unwrap_or_default();
    for dir in path.split(':') {
        if dir.is_empty() {
            continue;
        }
        let cand = PathBuf::from(dir).join("uml_mconsole");
        if cand.exists() {
            return Some(cand);
        }
    }
    None
}

/// POSIX shell-quote a single argument.  Sufficient for piping
/// argv through `uml_mconsole exec`; not for full shell expansion.
/// Public-in-module for tests.
fn shell_quote(s: &str) -> String {
    if !s.is_empty()
        && s.chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '/' | '.' | '=' | ':'))
    {
        return s.to_string();
    }
    let mut out = String::with_capacity(s.len() + 2);
    out.push('\'');
    for c in s.chars() {
        if c == '\'' {
            out.push_str("'\\''");
        } else {
            out.push(c);
        }
    }
    out.push('\'');
    out
}

/// Tiny shim so `do_exec` can read the signal field without pulling
/// in std::os::unix::process::ExitStatusExt at every call site.
trait ExitStatusSignal {
    fn signal_from_status(&self) -> Option<i32>;
}
impl ExitStatusSignal for std::process::ExitStatus {
    fn signal_from_status(&self) -> Option<i32> {
        use std::os::unix::process::ExitStatusExt;
        self.signal()
    }
}

// Suppress dead-code warning while replenish is wired-but-unused.
#[allow(dead_code)]
fn replenish_warm_pool(_state: &DaemonState) {
    // TODO Phase 1c-proper: pre-take up to min_warm members without a
    // corresponding RPC, park them in a "warm" queue, and pop on
    // take.  The fork primitive is identical to do_take; the
    // bookkeeping is a separate set keyed by pid.
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_take() {
        let req = parse_request(
            r#"{"op":"take","instance":"m1","mac":"52:54:00:aa:bb:cc",
                "tap":"tap-m1","ipv4":"10.7.0.42/24","gateway":"10.7.0.1",
                "mconsole":""}"#,
        )
        .unwrap();
        match req {
            Request::Take {
                instance,
                mac,
                tap,
                ipv4,
                gateway,
                mconsole,
            } => {
                assert_eq!(instance, "m1");
                assert_eq!(mac, "52:54:00:aa:bb:cc");
                assert_eq!(tap, "tap-m1");
                assert_eq!(ipv4, "10.7.0.42/24");
                assert_eq!(gateway, "10.7.0.1");
                assert_eq!(mconsole, "");
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn parse_take_defaults() {
        // Empty payload — only `op` present.  Defaults exercise serde's
        // `#[serde(default)]` so the client doesn't have to send every
        // field for an anonymous take.
        let req = parse_request(r#"{"op":"take"}"#).unwrap();
        assert!(matches!(req, Request::Take { .. }));
    }

    #[test]
    fn parse_other_ops() {
        assert!(matches!(
            parse_request(r#"{"op":"list"}"#).unwrap(),
            Request::List
        ));
        assert!(matches!(
            parse_request(r#"{"op":"status"}"#).unwrap(),
            Request::Status
        ));
        assert!(matches!(
            parse_request(r#"{"op":"shutdown"}"#).unwrap(),
            Request::Shutdown
        ));
        let d = parse_request(r#"{"op":"destroy","pid":12345}"#).unwrap();
        match d {
            Request::Destroy { pid } => assert_eq!(pid, 12345),
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn parse_garbage_rejected() {
        assert!(parse_request("").is_err());
        assert!(parse_request("not json").is_err());
        assert!(parse_request(r#"{"op":"bogus"}"#).is_err());
        assert!(parse_request(r#"{"op":"destroy"}"#).is_err()); // missing pid
    }

    #[test]
    fn parse_exec_request() {
        let r = parse_request(
            r#"{"op":"exec","pid":1234,"argv":["/bin/sh","-c","echo hi"],"timeout_secs":30}"#,
        )
        .unwrap();
        match r {
            Request::Exec {
                pid,
                argv,
                timeout_secs,
                ..
            } => {
                assert_eq!(pid, 1234);
                assert_eq!(argv, vec!["/bin/sh", "-c", "echo hi"]);
                assert_eq!(timeout_secs, 30);
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn parse_exec_requires_argv() {
        // argv is required (no #[serde(default)]); missing → error.
        assert!(parse_request(r#"{"op":"exec","pid":1}"#).is_err());
    }

    #[test]
    fn shell_quote_safe_chars_unchanged() {
        assert_eq!(shell_quote("hello"), "hello");
        assert_eq!(shell_quote("/usr/bin/sh"), "/usr/bin/sh");
        assert_eq!(shell_quote("FOO=bar"), "FOO=bar");
        assert_eq!(shell_quote("a-b_c.d"), "a-b_c.d");
    }

    #[test]
    fn shell_quote_special_chars_wrapped() {
        assert_eq!(shell_quote(""), "''");
        assert_eq!(shell_quote("hello world"), "'hello world'");
        assert_eq!(shell_quote("it's"), "'it'\\''s'");
        assert_eq!(shell_quote("a;b|c&d"), "'a;b|c&d'");
    }

    #[test]
    fn path_layout() {
        let p = Path::new("/run/uml");
        assert_eq!(
            pool_dir(p, "default"),
            PathBuf::from("/run/uml/pools/default")
        );
        assert_eq!(
            pool_socket(p, "default"),
            PathBuf::from("/run/uml/pools/default/api.sock")
        );
        assert_eq!(
            pool_pidfile(p, "default"),
            PathBuf::from("/run/uml/pools/default/serve.pid")
        );
    }
}
