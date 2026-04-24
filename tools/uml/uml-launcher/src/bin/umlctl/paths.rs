// SPDX-License-Identifier: GPL-2.0
//
// XDG Base Directory resolution for umlctl.
//
// The split matters: persistent state (manifests, logs,
// history) lives under $XDG_STATE_HOME so it survives reboot,
// and runtime state (pidfiles, reserved sockets) lives under
// $XDG_RUNTIME_DIR which is tmpfs on a modern systemd host.
// That way a stale pid from a prior boot can never be mistaken
// for a live instance — correctness, not tidiness.

use anyhow::{Context, Result};
use std::path::{Path, PathBuf};

pub struct Paths {
    pub state_dir: PathBuf,
    pub runtime_dir: PathBuf,
}

impl Paths {
    pub fn resolve(
        state_dir: Option<PathBuf>,
        runtime_dir: Option<PathBuf>,
    ) -> Result<Self> {
        let state_dir = state_dir.unwrap_or_else(default_state_dir);
        let runtime_dir = runtime_dir.unwrap_or_else(default_runtime_dir);

        // Create everything eagerly; ENOENT during a later verb
        // becomes "broken install" noise we'd rather avoid.
        std::fs::create_dir_all(&state_dir)
            .with_context(|| format!("create state_dir {}", state_dir.display()))?;
        std::fs::create_dir_all(state_dir.join("instances"))
            .context("create instances dir")?;
        std::fs::create_dir_all(state_dir.join("logs"))
            .context("create logs dir")?;
        std::fs::create_dir_all(&runtime_dir)
            .with_context(|| format!("create runtime_dir {}", runtime_dir.display()))?;

        Ok(Self { state_dir, runtime_dir })
    }

    pub fn instances_dir(&self) -> PathBuf {
        self.state_dir.join("instances")
    }

    pub fn manifest_path(&self, name: &str) -> PathBuf {
        self.instances_dir().join(format!("{name}.toml"))
    }

    pub fn logs_dir(&self) -> PathBuf {
        self.state_dir.join("logs")
    }

    pub fn history_path(&self) -> PathBuf {
        self.state_dir.join("history.jsonl")
    }

    pub fn pidfile_path(&self, name: &str) -> PathBuf {
        self.runtime_dir.join(format!("{name}.pid"))
    }
}

fn default_state_dir() -> PathBuf {
    if let Some(v) = std::env::var_os("XDG_STATE_HOME") {
        let p: &Path = Path::new(&v);
        if !v.is_empty() && p.is_absolute() {
            return p.join("uml");
        }
    }
    if let Some(home) = std::env::var_os("HOME") {
        return PathBuf::from(home).join(".local/state/uml");
    }
    PathBuf::from("/tmp/uml-state")
}

fn default_runtime_dir() -> PathBuf {
    if let Some(v) = std::env::var_os("XDG_RUNTIME_DIR") {
        let p: &Path = Path::new(&v);
        if !v.is_empty() && p.is_absolute() {
            return p.join("uml");
        }
    }
    // Per-UID fallback; private-ish for multi-user hosts.
    let uid = unsafe { libc::geteuid() };
    PathBuf::from(format!("/tmp/uml-{uid}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_and_pid_path_shape() {
        let p = Paths {
            state_dir: PathBuf::from("/s"),
            runtime_dir: PathBuf::from("/r"),
        };
        assert_eq!(p.manifest_path("foo"), PathBuf::from("/s/instances/foo.toml"));
        assert_eq!(p.pidfile_path("foo"), PathBuf::from("/r/foo.pid"));
        assert_eq!(p.logs_dir(), PathBuf::from("/s/logs"));
        assert_eq!(p.history_path(), PathBuf::from("/s/history.jsonl"));
    }
}
