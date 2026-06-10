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
// `timeout ./linux ...` shortcuts.

use anyhow::{anyhow, bail, Context, Result};
use std::os::fd::RawFd;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use super::cgroup;
use super::console_split;
use super::dmesg_parse;
use super::manifest::Manifest;
use super::paths::Paths;
use super::preflight;
use super::run;
use super::{StartArgs, StopArgs};

pub enum StartError {
    AlreadyRunning {
        pid: u32,
    },
    KernelMissing(PathBuf),
    ReadyTimeout {
        pid: u32,
        run_id: String,
        log_path: Option<PathBuf>,
    },
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
/// history.jsonl.
pub struct StartOutcome {
    pub pid: u32,
    pub run_id: String,
}

#[derive(Clone, Debug)]
pub struct InheritedFd {
    pub source_fd: RawFd,
    pub target_fd: RawFd,
}

#[allow(dead_code)]
pub fn start(
    paths: &Paths,
    m: &Manifest,
    args: &StartArgs,
) -> std::result::Result<StartOutcome, StartError> {
    start_with_fds(paths, m, args, &[])
}

pub fn start_with_fds(
    paths: &Paths,
    m: &Manifest,
    args: &StartArgs,
    inherited_fds: &[InheritedFd],
) -> std::result::Result<StartOutcome, StartError> {
    if !m.kernel.path.exists() {
        return Err(StartError::KernelMissing(m.kernel.path.clone()));
    }

    let pidfile = paths.pidfile_path(&args.name);

    // If the pidfile already has a live pid, refuse. Stale
    // pidfiles (process gone OR pid reused by an unrelated
    // process) are cleared and we proceed.
    if let Some(ident) = read_pidfile(&pidfile) {
        if identity_alive(ident) {
            return Err(StartError::AlreadyRunning { pid: ident.pid });
        }
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
    }

    // Mint a run_id up front - the bundle directory is
    // created pre-spawn so the log file we're about to
    // redirect into already has its permanent home.
    let run_id = run::generate_run_id();
    let run_dir = paths.run_dir(&run_id);
    std::fs::create_dir_all(&run_dir)
        .with_context(|| format!("create run dir {}", run_dir.display()))
        .map_err(StartError::Other)?;

    let argv = build_kernel_argv(m);

    // stdout + stderr are merged into init.log inside the bundle.
    // Stop derives a `kernel.log` sidecar by filtering init.log for
    // `[<ts>] ...` and
    // `<N>...` printk-shape lines; the merged file stays
    // authoritative because UML's init process shares the same
    // console fd that the kernel's printk subsystem writes to.
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

    let mut cmd = if let Some(strace_log) = &args.strace_log {
        if let Some(parent) = strace_log.parent() {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("create strace log dir {}", parent.display()))
                .map_err(StartError::Other)?;
        }
        let mut cmd = Command::new("strace");
        cmd.arg("-f")
            .arg("-s")
            .arg("256")
            .arg("-o")
            .arg(strace_log)
            .arg(&m.kernel.path);
        cmd
    } else {
        Command::new(&m.kernel.path)
    };
    cmd.args(&argv).stdout(stdout_cfg).stderr(stderr_cfg);

    // Apply host_env from the manifest. Each entry becomes a UM_*
    // env var read by os-Linux/main.c /
    // os-Linux/process.c at UML startup (UM_THP / UM_OOM_SCORE_ADJ /
    // UM_KVM_V2_CPU_AFFINITY / UM_HUGEPAGES / UM_KVM_V2_PIN_PHYSMEM).
    // Empty map = no-op (inherit caller's env unchanged).
    for (k, v) in &m.host_env {
        cmd.env(k, v);
    }

    if args.foreground {
        // Foreground: parent tty stays in the loop; Ctrl-C
        // travels through the normal tty pgrp path.
        cmd.stdin(Stdio::inherit());
    } else {
        cmd.stdin(Stdio::null());
    }
    install_pre_exec(&mut cmd, !args.foreground, inherited_fds);

    // Preflight resource verification.  Each warning is logged but
    // the run proceeds: kernel-side fallbacks handle missing hugepage
    // pools and cgroup writability gracefully.
    if !m.host_env.is_empty() || m.cgroup_v2.is_some() {
        if let Ok(report) = preflight::run(&m.host_env, m.cgroup_v2.as_ref()) {
            report.print();
        }
    }

    // If the manifest has a cgroup_v2 config, create the per-instance
    // cgroup with limits BEFORE spawn. We move the pid in after spawn
    // returns. Failure is not fatal: ensure_cgroup logs and returns None
    // when the
    // cgroup hierarchy isn't writable, and we proceed
    // unconstrained.
    let cgroup_path = m
        .cgroup_v2
        .as_ref()
        .and_then(|cfg| cgroup::ensure_cgroup(&args.name, cfg).ok().flatten());

    // Capture CLOCK_BOOTTIME immediately before spawn so the
    // recorded host_ts_ns_at_exec is as close as we can make it
    // to the kernel's own view of the child's start time.
    let host_ts_ns_at_exec = run::boottime_ns();
    let mut child = cmd
        .spawn()
        .with_context(|| format!("spawn {}", m.kernel.path.display()))
        .map_err(StartError::Other)?;
    let pid = match &args.strace_log {
        Some(strace_log) => wait_for_strace_tracee_pid(strace_log, Duration::from_secs(5))
            .unwrap_or_else(|| child.id()),
        None => child.id(),
    };

    // Move the freshly-spawned pid into the cgroup so the limits take
    // effect.
    if let Some(cg) = &cgroup_path {
        cgroup::move_pid_in(cg, pid);
    }

    write_pidfile(&pidfile, pid).map_err(StartError::Other)?;
    write_run_id_file(&paths.run_id_file_path(&args.name), &run_id).map_err(StartError::Other)?;
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
        derive_kernel_log_best_effort(paths, &run_id);
        parse_dmesg_best_effort(paths, &run_id, &args.name);
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
        return Ok(StartOutcome { pid, run_id });
    }

    // Detach mode: poll the log file for a ready marker up to
    // --ready-timeout. Fall back to "trust the spawn" if no
    // log file was created.
    //
    // Early-exit detection uses the owned Child handle's try_wait()
    // rather than
    // kill(pid, 0). A quickly-exited but unreaped child shows
    // up as live to kill(pid, 0) since the zombie is still in
    // the process table under our pid; try_wait() transparently
    // reaps it and returns Ok(Some(status)), which IS the
    // "child exited" signal we want. Falls back to the old
    // kill(pid, 0) probe only on the post-detach path below,
    // where the Child handle has been dropped.
    if let Some(lp) = &log_path {
        let deadline = Instant::now() + Duration::from_secs(args.ready_timeout);
        while Instant::now() < deadline {
            if log_indicates_ready(lp) {
                return Ok(StartOutcome { pid, run_id });
            }
            match child.try_wait() {
                Ok(Some(_status)) => {
                    run::finalize_run(
                        paths,
                        &run_id,
                        run::boottime_ns(),
                        "SPAWN_EXITED_EARLY",
                        _status.code(),
                    );
                    let _ = std::fs::remove_file(&pidfile);
                    let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
                    return Err(StartError::Other(anyhow!(
                        "kernel exited before reaching ready marker (see {})",
                        lp.display()
                    )));
                }
                Ok(None) => {}
                Err(e) => {
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
                        "waitpid on spawned child failed: {e}"
                    )));
                }
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        // Timeout expired - kill the child so the caller doesn't
        // leave it wedged.
        let _ = nix::sys::signal::kill(
            nix::unistd::Pid::from_raw(pid as i32),
            nix::sys::signal::Signal::SIGKILL,
        );
        let status = child.wait().ok();
        let code = status.and_then(|s| s.code());
        run::finalize_run(paths, &run_id, run::boottime_ns(), "READY_TIMEOUT", code);
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
        return Err(StartError::ReadyTimeout {
            pid,
            run_id,
            log_path: Some(lp.clone()),
        });
    }

    Ok(StartOutcome { pid, run_id })
}

fn install_pre_exec(cmd: &mut Command, detach: bool, inherited_fds: &[InheritedFd]) {
    let inherited_fds = inherited_fds.to_vec();

    // Safety: pre_exec runs post-fork / pre-exec. The closure only uses
    // async-signal-safe syscalls: setsid, dup2, and fcntl.
    unsafe {
        cmd.pre_exec(move || {
            if detach && libc::setsid() < 0 {
                return Err(std::io::Error::last_os_error());
            }

            for fd in &inherited_fds {
                if fd.source_fd < 0 || fd.target_fd < 0 {
                    return Err(std::io::Error::from_raw_os_error(libc::EBADF));
                }
                if fd.source_fd != fd.target_fd && libc::dup2(fd.source_fd, fd.target_fd) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
                let flags = libc::fcntl(fd.target_fd, libc::F_GETFD);
                if flags < 0 {
                    return Err(std::io::Error::last_os_error());
                }
                if libc::fcntl(fd.target_fd, libc::F_SETFD, flags & !libc::FD_CLOEXEC) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
            }

            Ok(())
        });
    }
}

pub fn stop(paths: &Paths, args: &StopArgs) -> std::result::Result<StopInfo, StopError> {
    let manifest_path = paths.manifest_path(&args.name);
    if !manifest_path.exists() {
        return Err(StopError::ManifestMissing);
    }
    let pidfile = paths.pidfile_path(&args.name);
    let ident = match read_pidfile(&pidfile) {
        Some(p) => p,
        None => return Err(StopError::NotRunning),
    };
    let pid = ident.pid;
    if !identity_alive(ident) {
        // Stale or reused pidfile; clear it so the next
        // start/stop sees a clean slate. Also drop the run_id
        // side-file.
        let _ = std::fs::remove_file(&pidfile);
        let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));
        return Err(StopError::NotRunning);
    }

    // Run_id side-file is best-effort - a missing/corrupt
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
        if !identity_alive(ident) {
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }

    if identity_alive(ident) && !args.force {
        // Escalate. We already sent the requested signal; now
        // send SIGKILL and give it a short window to settle.
        let _ = nix::sys::signal::kill(
            nix::unistd::Pid::from_raw(pid as i32),
            nix::sys::signal::Signal::SIGKILL,
        );
        let kill_deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < kill_deadline {
            if !identity_alive(ident) {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    // Drain any zombies of our own (helper procs we may have
    // spawned in this invocation). The UML itself isn't our
    // child post-start-detach, so this is cheap no-op on the
    // common path - it's a safety net for future helpers.
    drain_zombies();

    // Finalize the run bundle with our exit-side fields.
    // exit_status stays None on the detach path because the
    // UML isn't our child post-start; a control socket can surface a
    // real one via guest-agent shutdown handshakes.
    if !run_id.is_empty() {
        run::finalize_run(paths, &run_id, run::boottime_ns(), &signal_name, None);
        derive_kernel_log_best_effort(paths, &run_id);
        parse_dmesg_best_effort(paths, &run_id, &args.name);
    }

    let _ = std::fs::remove_file(&pidfile);
    let _ = std::fs::remove_file(paths.run_id_file_path(&args.name));

    // Teardown the per-instance cgroup. Safe to call even if no
    // cgroup was created at start (no-op on missing dir). EBUSY
    // (still-non-empty cgroup) is logged and ignored.
    cgroup::teardown(&args.name);

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
        Some(ident) => identity_alive(ident),
        None => false,
    }
}

/// Pidfile identity.  Carries both the pid and
/// /proc/<pid>/stat's starttime_ticks.  `starttime = None` means the
/// pidfile uses the single-line `<pid>\n` compatibility format.
#[derive(Copy, Clone)]
pub(super) struct PidIdentity {
    pub pid: u32,
    pub starttime: Option<u64>,
}

/// Read /proc/<pid>/stat field 22 (starttime, clock ticks since
/// boot). Returns None if /proc/<pid>/stat doesn't exist OR can't be
/// parsed; the caller then disables the PID-reuse check for that call
/// site only.
pub(super) fn read_starttime(pid: u32) -> Option<u64> {
    let s = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let rparen = s.rfind(')')?;
    let rest = s.get(rparen + 1..)?.trim_start();
    let fields: Vec<&str> = rest.split_whitespace().collect();
    fields.get(19)?.parse().ok()
}

pub(super) fn read_pidfile(path: &Path) -> Option<PidIdentity> {
    let s = std::fs::read_to_string(path).ok()?;
    // Current pidfile format is "<pid> <starttime>\n"; accept the
    // older "<pid>\n" shape for backward compatibility.
    let trimmed = s.trim();
    let mut it = trimmed.split_whitespace();
    let pid: u32 = it.next()?.parse().ok()?;
    let starttime = it.next().and_then(|t| t.parse::<u64>().ok());
    Some(PidIdentity { pid, starttime })
}

fn write_pidfile(path: &Path, pid: u32) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create runtime dir {}", parent.display()))?;
    }
    // Current format: "<pid> <starttime>\n". Starttime is read from
    // /proc/<pid>/stat right after spawn; a missing value
    // (child raced away before we could read stat) is fine -
    // we fall back to the "<pid>\n" format on that row
    // and the corresponding read_pidfile returns
    // starttime=None. Subsequent identity checks for that
    // pidfile fall back to kill(pid, 0) semantics with a one-
    // time warn, keeping backward compat with stale
    // post-upgrade state.
    let contents = match read_starttime(pid) {
        Some(st) => format!("{pid} {st}\n"),
        None => format!("{pid}\n"),
    };
    std::fs::write(path, contents).with_context(|| format!("write pidfile {}", path.display()))?;
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
    // alive - treat as running). Everything else counts as dead.
    //
    // NOTE: this helper is PID-only and therefore vulnerable to
    // pid-reuse races. Prefer identity_alive() which also checks
    // /proc/<pid>/stat starttime against a captured baseline.
    // process_alive() is retained for pidfiles that never
    // recorded a starttime; identity_alive() calls through to it
    // as the fallback.
    unsafe {
        let r = libc::kill(pid as libc::pid_t, 0);
        if r == 0 {
            return true;
        }
        let err = *libc::__errno_location();
        err == libc::EPERM
    }
}

/// PID-reuse-safe liveness probe. A raw kill(pid, 0) can't distinguish
/// "the process we
/// originally spawned is still running" from "that pid was
/// recycled by the kernel for an unrelated process." We defend
/// by comparing /proc/<pid>/stat starttime_ticks against the
/// value we recorded at spawn; the starttime field is monotone
/// per-pid (set at fork-time, never changed), so a mismatch
/// means pid-reuse.
///
/// If `identity.starttime` is None (older pidfile), falls back
/// to the PID-only check with a one-shot warn - this only fires
/// for compatibility-format pidfiles and disappears after the next
/// stop+start cycle.
pub(super) fn identity_alive(identity: PidIdentity) -> bool {
    if !process_alive(identity.pid) {
        return false;
    }
    match identity.starttime {
        Some(want) => match read_starttime(identity.pid) {
            Some(got) => got == want,
            None => {
                /*
                 * /proc/<pid>/stat disappeared between the
                 * kill(pid, 0) probe and the starttime read
                 * - unlikely but possible. Treat as "dead"
                 * since we can't confirm identity.
                 */
                false
            }
        },
        None => {
            use std::sync::atomic::{AtomicBool, Ordering};
            static WARNED: AtomicBool = AtomicBool::new(false);
            if !WARNED.swap(true, Ordering::Relaxed) {
                eprintln!(
                    "umlctl: warning: pidfile lacks birth marker; \
                          pid-reuse race possible on this instance. \
                          restart the instance to rewrite it."
                );
            }
            true
        }
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

fn read_first_strace_pid(path: &Path) -> Option<u32> {
    let s = std::fs::read_to_string(path).ok()?;
    for line in s.lines() {
        let end = line
            .find(|c: char| !c.is_ascii_digit())
            .unwrap_or(line.len());
        if end == 0 {
            continue;
        }
        if let Ok(pid) = line[..end].parse::<u32>() {
            return Some(pid);
        }
    }
    None
}

fn wait_for_strace_tracee_pid(path: &Path, timeout: Duration) -> Option<u32> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Some(pid) = read_first_strace_pid(path) {
            return Some(pid);
        }
        std::thread::sleep(Duration::from_millis(25));
    }
    read_first_strace_pid(path)
}

/// Derive `kernel.log` next to `init.log` in the run bundle.
/// Best-effort: any I/O error is swallowed so a malformed log
/// can't poison the stop path (the merged init.log stays
/// authoritative either way).
fn derive_kernel_log_best_effort(paths: &Paths, run_id: &str) {
    let dir = paths.run_dir(run_id);
    let init_log = dir.join("init.log");
    let kernel_log = dir.join("kernel.log");
    let _ = console_split::derive_kernel_log(&init_log, &kernel_log);
}

/// Scan the derived `kernel.log` for sanitizer/panic/oom
/// splats and append one structured event per match to
/// `events.jsonl`. Best-effort on the stop path - a parser
/// hiccup must never block the signal-and-reap sequence.
fn parse_dmesg_best_effort(paths: &Paths, run_id: &str, instance: &str) {
    let kernel_log = paths.run_dir(run_id).join("kernel.log");
    let _ = dmesg_parse::parse_and_emit(paths, run_id, instance, &kernel_log);
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
        "ubd" => {
            // ubd-rooted instance (umlbuild-emitted Umlfile): the
            // Umlfile's [kernel].append carries the actual
            // ubd0=PATH + root=/dev/ubda + rw tokens, so emit
            // nothing here.  Adding `root=ubd` would be a literal
            // unknown-device the kernel would warn about.
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
                created_at: "1970-01-01T00:00:00Z".into(),
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
            host_env: Default::default(),
            cgroup_v2: None,
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

    #[test]
    fn strace_pid_parser_uses_first_tracee_pid() {
        let path = std::env::temp_dir().join(format!(
            "umlctl-strace-pid-parser-{}-{}.log",
            std::process::id(),
            run::generate_run_id()
        ));
        std::fs::write(
            &path,
            "\n3797038 execve(\"/tmp/linux\", [\"linux\"], 0x7fff) = 0\n3797039 brk(NULL) = 0x1\n",
        )
        .unwrap();

        assert_eq!(read_first_strace_pid(&path), Some(3797038));

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn strace_pid_parser_ignores_non_pid_prefixes() {
        let path = std::env::temp_dir().join(format!(
            "umlctl-strace-pid-parser-nonpid-{}-{}.log",
            std::process::id(),
            run::generate_run_id()
        ));
        std::fs::write(
            &path,
            "strace: Process 42 attached\n4242 mmap(NULL, 4096, PROT_READ) = 0\n",
        )
        .unwrap();

        assert_eq!(read_first_strace_pid(&path), Some(4242));

        let _ = std::fs::remove_file(path);
    }
}
