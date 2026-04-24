// SPDX-License-Identifier: GPL-2.0
//
// Lifecycle: start (fork + detach + ready-wait), stop (signal +
// poll + escalate), is_running (pidfile + kill(0) probe).
//
// This is the module that closes the runaway-UML-process hole:
// `umlctl stop` sends an explicit signal to the pid recorded in
// the pidfile and polls `kill(pid, 0)` until it returns ESRCH.
// If the initial signal doesn't land inside --timeout, we
// escalate to SIGKILL. No more orphan accumulation from
// `timeout ./linux …` shortcuts.

use anyhow::{anyhow, bail, Context, Result};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use super::manifest::Manifest;
use super::paths::Paths;
use super::run;
use super::{StartArgs, StopArgs};

pub enum StartError {
    AlreadyRunning { pid: u32 },
    KernelMissing(PathBuf),
    ReadyTimeout,
    Other(anyhow::Error),
}

pub enum StopError {
    ManifestMissing,
    NotRunning,
    Other(anyhow::Error),
}

pub struct StopInfo {
    pub pid: u32,
    pub signal_sent: String,
    pub exit_status: Option<i32>,
    pub run_id: String,
}

/// Descriptor of a just-started run, threaded back to the
/// command layer so umlctl can print `run_id` + record it in
/// history.jsonl. Supersedes the earlier "just return a pid"
/// signature now that the observability spine (memo 13) needs
/// every start to produce a correlation id.
pub struct StartOutcome {
    pub pid: u32,
    pub run_id: String,
}

pub fn start(
    paths: &Paths,
    m: &Manifest,
    args: &StartArgs,
) -> std::result::Result<StartOutcome, StartError> {
    if !m.kernel.path.exists() {
        return Err(StartError::KernelMissing(m.kernel.path.clone()));
    }

    let pidfile = paths.pidfile_path(&args.name);

    // If the pidfile already has a live pid, refuse. Stale
    // pidfiles (process gone) are cleared and we proceed.
    if let Some(pid) = read_pidfile(&pidfile) {
        if process_alive(pid) {
            return Err(StartError::AlreadyRunning { pid });
        }
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
    }

    // Mint a run_id up front — the bundle directory is
    // created pre-spawn so the log file we're about to
    // redirect into already has its permanent home.
    let run_id = run::generate_run_id();
    let run_dir = paths.run_dir(&run_id);
    std::fs::create_dir_all(&run_dir)
        .with_context(|| format!("create run dir {}", run_dir.display()))
        .map_err(StartError::Other)?;

    let argv = build_kernel_argv(m);

    // v1 layout: stdout + stderr merged into init.log inside
    // the bundle. Kernel console split to its own file lands
    // in O1.2.
    let log_path = if args.no_log {
        None
    } else {
        Some(run_dir.join("init.log"))
    };

    let (stdout_cfg, stderr_cfg) = match &log_path {
        Some(p) => {
            let f = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(p)
                .with_context(|| format!("open log {}", p.display()))
                .map_err(StartError::Other)?;
            let f2 = f.try_clone().map_err(|e| StartError::Other(e.into()))?;
            (Stdio::from(f), Stdio::from(f2))
        }
        None => (Stdio::null(), Stdio::null()),
    };

    let mut cmd = Command::new(&m.kernel.path);
    cmd.args(&argv).stdout(stdout_cfg).stderr(stderr_cfg);

    if args.foreground {
        // Foreground: parent tty stays in the loop; Ctrl-C
        // travels through the normal tty pgrp path.
        cmd.stdin(Stdio::inherit());
    } else {
        cmd.stdin(Stdio::null());
        // Detach via setsid so SIGHUP on the shell's tty
        // doesn't drag the UML down when umlctl exits.
        // Safety: pre_exec runs post-fork / pre-exec; setsid
        // is async-signal-safe.
        unsafe {
            cmd.pre_exec(|| match nix::unistd::setsid() {
                Ok(_) => Ok(()),
                Err(e) => Err(std::io::Error::from_raw_os_error(e as i32)),
            });
        }
    }

    // Capture CLOCK_BOOTTIME immediately before spawn so the
    // recorded host_ts_ns_at_exec is as close as we can make it
    // to the kernel's own view of the child's start time.
    let host_ts_ns_at_exec = run::boottime_ns();
    let mut child = cmd
        .spawn()
        .with_context(|| format!("spawn {}", m.kernel.path.display()))
        .map_err(StartError::Other)?;
    let pid = child.id();

    write_pidfile(&pidfile, pid).map_err(StartError::Other)?;
    write_run_id_file(&paths.run_id_file_path(&args.name), &run_id)
        .map_err(StartError::Other)?;
    run::create_run_bundle(
        paths,
        &run_id,
        &args.name,
        &m.kernel.sha256,
        host_ts_ns_at_exec,
        pid,
    )
    .map_err(StartError::Other)?;

    if args.foreground {
        // Block in-process. Once the child exits, remove the
        // pidfile so ps doesn't show it as running and finalize
        // the run bundle with whatever exit status we observed.
        let status = child.wait().ok();
        let code = status.and_then(|s| s.code());
        run::finalize_run(paths, &run_id, run::boottime_ns(), "FOREGROUND_EXIT", code);
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
        return Ok(StartOutcome { pid, run_id });
    }

    // Detach mode: poll the log file for a ready marker up to
    // --ready-timeout. Fall back to "trust the spawn" if no
    // log file was created.
    if let Some(lp) = &log_path {
        let deadline = Instant::now() + Duration::from_secs(args.ready_timeout);
        while Instant::now() < deadline {
            if log_indicates_ready(lp) {
                return Ok(StartOutcome { pid, run_id });
            }
            if !process_alive(pid) {
                run::finalize_run(
                    paths,
                    &run_id,
                    run::boottime_ns(),
                    "SPAWN_EXITED_EARLY",
                    None,
                );
                let _ = std::fs::remove_file(&pidfile);
                let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
                return Err(StartError::Other(anyhow!(
                    "kernel exited before reaching ready marker (see {})",
                    lp.display()
                )));
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        // Timeout expired — kill the child so the caller doesn't
        // leave it wedged.
        let _ = nix::sys::signal::kill(
            nix::unistd::Pid::from_raw(pid as i32),
            nix::sys::signal::Signal::SIGKILL,
        );
        run::finalize_run(paths, &run_id, run::boottime_ns(), "READY_TIMEOUT", None);
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
        return Err(StartError::ReadyTimeout);
    }

    Ok(StartOutcome { pid, run_id })
}

pub fn stop(paths: &Paths, args: &StopArgs) -> std::result::Result<StopInfo, StopError> {
    let manifest_path = paths.manifest_path(&args.name);
    if !manifest_path.exists() {
        return Err(StopError::ManifestMissing);
    }
    let pidfile = paths.pidfile_path(&args.name);
    let pid = match read_pidfile(&pidfile) {
        Some(p) => p,
        None => return Err(StopError::NotRunning),
    };
    if !process_alive(pid) {
        // Stale pidfile; clear it so the next start/stop sees
        // a clean slate. Also drop the run_id side-file.
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
        return Err(StopError::NotRunning);
    }

    // Run_id side-file is best-effort — a missing/corrupt
    // one shouldn't block a stop. Fall back to the empty
    // string so history still records the event.
    let run_id = read_run_id_file(&paths.run_id_file_path(&args.name)).unwrap_or_default();

    let (sig, signal_name) = if args.force {
        (nix::sys::signal::Signal::SIGKILL, "KILL".to_string())
    } else {
        parse_signal(&args.signal).map_err(StopError::Other)?
    };

    nix::sys::signal::kill(nix::unistd::Pid::from_raw(pid as i32), sig)
        .map_err(|e| StopError::Other(anyhow!("kill({pid}, {sig:?}): {e}")))?;

    let deadline = Instant::now() + Duration::from_secs(args.timeout);
    while Instant::now() < deadline {
        if !process_alive(pid) {
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }

    if process_alive(pid) && !args.force {
        // Escalate. We already sent the requested signal; now
        // send SIGKILL and give it a short window to settle.
        let _ = nix::sys::signal::kill(
            nix::unistd::Pid::from_raw(pid as i32),
            nix::sys::signal::Signal::SIGKILL,
        );
        let kill_deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < kill_deadline {
            if !process_alive(pid) {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    // Drain any zombies of our own (helper procs we may have
    // spawned in this invocation). The UML itself isn't our
    // child post-start-detach, so this is cheap no-op on the
    // common path — it's a safety net for future helpers.
    drain_zombies();

    // Finalize the run bundle with our exit-side fields.
    // exit_status stays None on the detach path because the
    // UML isn't our child post-start; a later phase (control
    // socket) can surface a real one via guest-agent shutdown
    // handshakes.
    if !run_id.is_empty() {
        run::finalize_run(paths, &run_id, run::boottime_ns(), &signal_name, None);
    }

    let _ = std::fs::remove_file(&pidfile);
    let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));

    Ok(StopInfo {
        pid,
        signal_sent: signal_name,
        exit_status: None,
        run_id,
    })
}

pub fn is_running(paths: &Paths, name: &str) -> bool {
    let pidfile = paths.pidfile_path(name);
    match read_pidfile(&pidfile) {
        Some(pid) => process_alive(pid),
        None => false,
    }
}

pub(super) fn read_pidfile(path: &Path) -> Option<u32> {
    let s = std::fs::read_to_string(path).ok()?;
    s.trim().parse().ok()
}

fn write_pidfile(path: &Path, pid: u32) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create runtime dir {}", parent.display()))?;
    }
    std::fs::write(path, format!("{pid}\n"))
        .with_context(|| format!("write pidfile {}", path.display()))?;
    Ok(())
}

fn write_run_id_file(path: &Path, run_id: &str) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create runtime dir {}", parent.display()))?;
    }
    std::fs::write(path, format!("{run_id}\n"))
        .with_context(|| format!("write run_id file {}", path.display()))?;
    Ok(())
}

pub(super) fn read_run_id_file(path: &Path) -> Option<String> {
    let s = std::fs::read_to_string(path).ok()?;
    let s = s.trim();
    if s.is_empty() {
        None
    } else {
        Some(s.to_string())
    }
}

pub(super) fn process_alive(pid: u32) -> bool {
    // kill(pid, 0): 0 means live + we have perms. ESRCH means
    // dead for real. EPERM means live but not ours (still
    // alive — treat as running). Everything else counts as dead.
    unsafe {
        let r = libc::kill(pid as libc::pid_t, 0);
        if r == 0 {
            return true;
        }
        let err = *libc::__errno_location();
        err == libc::EPERM
    }
}

fn parse_signal(s: &str) -> Result<(nix::sys::signal::Signal, String)> {
    let up = s.trim().to_uppercase();
    let sig = match up.as_str() {
        "TERM" | "SIGTERM" => nix::sys::signal::Signal::SIGTERM,
        "KILL" | "SIGKILL" => nix::sys::signal::Signal::SIGKILL,
        "INT" | "SIGINT" => nix::sys::signal::Signal::SIGINT,
        "QUIT" | "SIGQUIT" => nix::sys::signal::Signal::SIGQUIT,
        other => bail!("unsupported signal {other:?} (supported: TERM, KILL, INT, QUIT)"),
    };
    let display = up.strip_prefix("SIG").unwrap_or(&up).to_string();
    Ok((sig, display))
}

fn drain_zombies() {
    use nix::sys::wait::{waitpid, WaitPidFlag, WaitStatus};
    use nix::unistd::Pid;
    loop {
        match waitpid(Pid::from_raw(-1), Some(WaitPidFlag::WNOHANG)) {
            Ok(WaitStatus::StillAlive) | Err(_) => break,
            // Including Exited / Signaled / Stopped / Continued:
            // keep draining until nothing's left.
            _ => continue,
        }
    }
}

fn log_indicates_ready(path: &Path) -> bool {
    // v1 readiness: any of the early-boot markers shows up.
    // UML's "Linux version" is printed very early; the init
    // markers confirm we're past the crash-before-userspace
    // window. Not perfect but good enough for detach; a v3
    // control-socket handshake replaces this.
    let Ok(s) = std::fs::read_to_string(path) else {
        return false;
    };
    s.contains("Linux version")
        || s.contains("Booting Linux")
        || s.contains("Run /sbin/init")
        || s.contains("Freeing unused kernel")
}

fn build_kernel_argv(m: &Manifest) -> Vec<String> {
    let mut argv = Vec::new();

    argv.push(format!("mem={}", m.runtime.mem));

    match m.runtime.root.as_str() {
        "hostfs" => {
            argv.push("rootfstype=hostfs".into());
            argv.push("rootflags=/".into());
            argv.push("root=/dev/root".into());
            argv.push("rw".into());
        }
        other => {
            // v1 otherwise-passthrough: let the caller hand-roll
            // `root=ubd0` etc. via the manifest's `root` field.
            argv.push(format!("root={other}"));
        }
    }

    if !m.kernel.backend.is_empty() {
        argv.push(format!("backend={}", m.kernel.backend));
    }

    if m.runtime.ncpus > 1 {
        argv.push(format!("ncpus={}", m.runtime.ncpus));
    }

    for tok in m.runtime.cmdline.split_whitespace() {
        argv.push(tok.to_string());
    }

    argv
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest_fixture() -> Manifest {
        Manifest {
            schema_version: 1,
            instance: super::super::manifest::InstanceSection {
                name: "foo".into(),
                created_at: "2026-04-23T00:00:00Z".into(),
            },
            kernel: super::super::manifest::KernelSection {
                path: PathBuf::from("/tmp/linux"),
                sha256: "deadbeef".into(),
                profile: "research".into(),
                backend: "seccomp".into(),
            },
            runtime: super::super::manifest::RuntimeSection {
                mem: "256M".into(),
                ncpus: 1,
                cmdline: "quiet foo=bar".into(),
                root: "hostfs".into(),
                forkserver: false,
            },
            labels: Default::default(),
        }
    }

    #[test]
    fn argv_hostfs() {
        let m = manifest_fixture();
        let a = build_kernel_argv(&m);
        assert!(a.iter().any(|s| s == "rootfstype=hostfs"));
        assert!(a.iter().any(|s| s == "root=/dev/root"));
        assert!(a.iter().any(|s| s == "mem=256M"));
        assert!(a.iter().any(|s| s == "backend=seccomp"));
        assert!(a.iter().any(|s| s == "quiet"));
        assert!(a.iter().any(|s| s == "foo=bar"));
    }

    #[test]
    fn argv_ncpus_only_emitted_when_gt1() {
        let mut m = manifest_fixture();
        m.runtime.ncpus = 4;
        let a = build_kernel_argv(&m);
        assert!(a.iter().any(|s| s == "ncpus=4"));

        m.runtime.ncpus = 1;
        let a = build_kernel_argv(&m);
        assert!(!a.iter().any(|s| s.starts_with("ncpus=")));
    }

    #[test]
    fn signal_parsing() {
        assert!(matches!(
            parse_signal("TERM").unwrap().0,
            nix::sys::signal::Signal::SIGTERM
        ));
        assert!(matches!(
            parse_signal("sigkill").unwrap().0,
            nix::sys::signal::Signal::SIGKILL
        ));
        assert!(parse_signal("HUP").is_err());
    }
}
