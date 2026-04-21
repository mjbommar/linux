// SPDX-License-Identifier: GPL-2.0
//
// Core launcher: build argv from Config, spawn UML, supervise
// the child, return its exit code.
//
// Intentionally small and synchronous. No async runtime; one
// child, one signal thread, one main thread doing .wait(). v2's
// per-device vhost-user helpers will pull in tokio — v1 doesn't
// need it.

use std::os::unix::process::CommandExt;
use std::process::Command;
use std::sync::Arc;

use anyhow::{anyhow, Context, Result};
use shared_child::SharedChild;

use crate::cli::Console;
use crate::config::Config;
use crate::signal;

/// Build + spawn + supervise UML. Returns its exit code.
pub fn run(cfg: Config) -> Result<i32> {
    let argv = build_argv(&cfg)?;

    if cfg.dry_run {
        println!("{} {}", cfg.kernel.display(), argv.join(" "));
        return Ok(0);
    }

    tracing::info!(
        kernel = %cfg.kernel.display(),
        "spawning UML kernel"
    );
    tracing::debug!(argv = ?argv, "kernel argv");

    let mut cmd = Command::new(&cfg.kernel);
    cmd.args(&argv);

    // Forkserver fd plumbing: dup2 the host-side ctl/status fds
    // into the UML child as fds 198/199 so the AFL protocol works
    // end-to-end. Must happen post-fork, pre-exec (pre_exec is
    // exactly that window).
    //
    // Safety: pre_exec runs between fork and execve. Only async-
    // signal-safe operations are allowed. dup2 and fcntl (via
    // nix) are in that set. The closure captures i32 fds by copy;
    // the source OwnedFds remain in the parent and get dropped
    // on the normal path.
    if let Some((ctl_fd, status_fd)) = cfg.forkserver {
        tracing::info!(ctl = ctl_fd, status = status_fd, "plumbing forkserver fds");
        unsafe {
            cmd.pre_exec(move || {
                use nix::fcntl::{fcntl, FcntlArg, FdFlag};
                use nix::unistd::dup2;

                // dup2 returns EINVAL if src == dst; handle that
                // edge for callers that already put the fds in
                // place (the kselftest driver does, for example).
                if ctl_fd != 198 {
                    dup2(ctl_fd, 198).map_err(std::io::Error::from)?;
                }
                if status_fd != 199 {
                    dup2(status_fd, 199).map_err(std::io::Error::from)?;
                }
                // Clear FD_CLOEXEC on both so they survive the
                // UML binary's internal dup/close dance.
                fcntl(198, FcntlArg::F_SETFD(FdFlag::empty()))
                    .map_err(std::io::Error::from)?;
                fcntl(199, FcntlArg::F_SETFD(FdFlag::empty()))
                    .map_err(std::io::Error::from)?;
                Ok(())
            });
        }
    }

    let child =
        SharedChild::spawn(&mut cmd).with_context(|| format!("spawning {:?}", cfg.kernel))?;
    let child = Arc::new(child);
    let pid = child.id();
    tracing::info!(pid, "UML child spawned");

    let sig_handle = signal::install_forwarder(Arc::clone(&child))
        .context("installing signal forwarder")?;

    let status = child
        .wait()
        .context("waiting for UML child")?;

    sig_handle.close();

    let code = status.code().unwrap_or_else(|| {
        // The child was signaled; pick the conventional
        // 128+signum mapping so shell exit-code introspection
        // works (same as bash).
        use std::os::unix::process::ExitStatusExt;
        status.signal().map(|s| 128 + s).unwrap_or(-1)
    });

    tracing::info!(pid, exit_code = code, "UML child exited");
    Ok(code)
}

/// Synthesize the UML kernel command-line argv from Config.
///
/// Shape: `rootfstype=hostfs rootflags=/ init=<path> mem=<size>
///         con=null con0=fd:0,fd:1 root=/dev/root rw
///         <any --append entries>`
///
/// Keeping this pure (no IO, no side effects) so it's table-
/// driven-testable.
pub fn build_argv(cfg: &Config) -> Result<Vec<String>> {
    let mut argv = Vec::with_capacity(16);

    match cfg.root.as_str() {
        "hostfs" => {
            argv.push("rootfstype=hostfs".into());
            argv.push("rootflags=/".into());
            argv.push("root=/dev/root".into());
            argv.push("rw".into());
        }
        other => {
            // v1 hostfs-only. Block-device roots land in v2 when
            // the ubd helper process ships.
            return Err(anyhow!(
                "root={other:?} not supported in v1 (hostfs only). Use --root hostfs."
            ));
        }
    }

    argv.push(format!("init={}", cfg.init.display()));
    argv.push(format!("mem={}", cfg.mem));

    match cfg.console {
        Console::Stdio => {
            // UML console on stdin/stdout, so an interactive
            // init works out of the box.
            argv.push("con=null".into());
            argv.push("con0=fd:0,fd:1".into());
            argv.push("console=tty".into());
        }
        Console::Null => {
            argv.push("con=null".into());
        }
    }

    argv.extend(cfg.append.iter().cloned());
    Ok(argv)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn cfg(init: &str, mem: &str) -> Config {
        Config {
            kernel: PathBuf::from("/tmp/linux"),
            init: PathBuf::from(init),
            mem: mem.to_string(),
            root: "hostfs".to_string(),
            console: Console::Stdio,
            forkserver: None,
            append: Vec::new(),
            dry_run: false,
        }
    }

    #[test]
    fn argv_hostfs_stdio() {
        let a = build_argv(&cfg("/bin/true", "256M")).unwrap();
        assert!(a.iter().any(|s| s == "rootfstype=hostfs"));
        assert!(a.iter().any(|s| s == "rootflags=/"));
        assert!(a.iter().any(|s| s == "init=/bin/true"));
        assert!(a.iter().any(|s| s == "mem=256M"));
        assert!(a.iter().any(|s| s == "con0=fd:0,fd:1"));
    }

    #[test]
    fn argv_null_console() {
        let mut c = cfg("/bin/true", "128M");
        c.console = Console::Null;
        let a = build_argv(&c).unwrap();
        assert!(a.iter().any(|s| s == "con=null"));
        assert!(!a.iter().any(|s| s == "con0=fd:0,fd:1"));
    }

    #[test]
    fn argv_append_appended() {
        let mut c = cfg("/bin/sh", "128M");
        c.append = vec!["quiet".into(), "foo=bar".into()];
        let a = build_argv(&c).unwrap();
        let tail: Vec<&String> = a.iter().rev().take(2).collect();
        // Appended items come at the end, in order.
        assert_eq!(tail[1], "quiet");
        assert_eq!(tail[0], "foo=bar");
    }

    #[test]
    fn argv_rejects_non_hostfs_root() {
        let mut c = cfg("/bin/sh", "128M");
        c.root = "ubd0".to_string();
        let err = build_argv(&c).unwrap_err();
        assert!(err.to_string().contains("not supported in v1"));
    }
}
