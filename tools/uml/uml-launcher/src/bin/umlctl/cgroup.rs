// SPDX-License-Identifier: GPL-2.0
//
// Cgroup v2 helpers for SMP-T83 (memo 52 §2.3 — UML host
// resource controls).
//
// Per-instance cgroup at /sys/fs/cgroup/uml.slice/<name>/.
// Writes memory.max / cpu.max / pids.max from the manifest's
// CgroupV2Config. Moves the UML pid into the cgroup post-spawn
// via cgroup.procs. Cleans up the empty cgroup at stop time.
//
// Gracefully no-ops if the cgroup hierarchy is unwritable
// (typical: dev rig without systemd Delegate=yes, or rootless
// invocation). Failure is logged; UML continues unconstrained.

use anyhow::Result;
use std::path::{Path, PathBuf};

use crate::manifest::CgroupV2Config;

const UML_SLICE_ROOT: &str = "/sys/fs/cgroup/uml.slice";

/// Build the per-instance cgroup path:
/// /sys/fs/cgroup/uml.slice/<instance-name>/
fn instance_cgroup_path(instance: &str) -> PathBuf {
    PathBuf::from(format!("{UML_SLICE_ROOT}/{instance}"))
}

/// Try to create the per-instance cgroup and write limits.
///
/// Returns Ok(Some(path)) on success — caller stashes the path
/// for later procs-write + cleanup.
/// Returns Ok(None) if the cgroup hierarchy is unavailable
/// (cgroup v1 / unmounted / unwritable) — UML proceeds
/// unconstrained.
/// Returns Err only on programmer error (never on operational
/// "cgroup not configured here" cases).
pub fn ensure_cgroup(instance: &str, cfg: &CgroupV2Config) -> Result<Option<PathBuf>> {
    let slice = Path::new(UML_SLICE_ROOT);

    // Probe: is cgroup v2 mounted at /sys/fs/cgroup?
    if !Path::new("/sys/fs/cgroup/cgroup.controllers").exists() {
        tracing::warn!(
            "SMP-T83 cgroup_v2: /sys/fs/cgroup/cgroup.controllers absent — \
             skipping cgroup setup for instance {instance}"
        );
        return Ok(None);
    }

    // Best-effort: create uml.slice and the per-instance subgroup.
    if std::fs::create_dir_all(slice).is_err() {
        tracing::warn!(
            "SMP-T83 cgroup_v2: cannot create {} (permission?) — \
             skipping cgroup setup for instance {instance}",
            slice.display()
        );
        return Ok(None);
    }
    let path = instance_cgroup_path(instance);
    if let Err(e) = std::fs::create_dir_all(&path) {
        tracing::warn!(
            "SMP-T83 cgroup_v2: cannot create {} ({e}) — \
             skipping cgroup setup",
            path.display()
        );
        return Ok(None);
    }

    // Enable required controllers on the parent (uml.slice). cgroup v2
    // requires the parent to enable a controller before children can
    // use it. Idempotent.
    let _ = std::fs::write(
        slice.join("cgroup.subtree_control"),
        "+memory +cpu +pids\n",
    );

    if !cfg.memory_max.is_empty() {
        if let Err(e) = std::fs::write(path.join("memory.max"), cfg.memory_max.as_bytes()) {
            tracing::warn!(
                "SMP-T83 memory.max write failed ({e}) — limit not applied"
            );
        }
    }
    if !cfg.cpu_max.is_empty() {
        if let Err(e) = std::fs::write(path.join("cpu.max"), cfg.cpu_max.as_bytes()) {
            tracing::warn!("SMP-T83 cpu.max write failed ({e}) — limit not applied");
        }
    }
    if let Some(n) = cfg.pids_max {
        let s = n.to_string();
        if let Err(e) = std::fs::write(path.join("pids.max"), s.as_bytes()) {
            tracing::warn!("SMP-T83 pids.max write failed ({e}) — limit not applied");
        }
    }

    Ok(Some(path))
}

/// Move @pid into the cgroup. Called after spawn. If write fails
/// (e.g. cgroup was removed under us), log and move on.
pub fn move_pid_in(cgroup_path: &Path, pid: u32) {
    let procs = cgroup_path.join("cgroup.procs");
    let s = pid.to_string();
    if let Err(e) = std::fs::write(&procs, s.as_bytes()) {
        tracing::warn!(
            "SMP-T83 cgroup.procs write failed ({e}) — pid {pid} \
             remains in parent cgroup"
        );
    } else {
        tracing::info!(
            "SMP-T83 cgroup_v2: moved pid {pid} into {}",
            cgroup_path.display()
        );
    }
}

/// Remove the per-instance cgroup. Caller invokes at stop time
/// after the UML pid has exited (cgroup must be empty). rmdir on
/// a non-empty cgroup returns EBUSY — log and move on.
pub fn teardown(instance: &str) {
    let path = instance_cgroup_path(instance);
    if !path.exists() {
        return;
    }
    if let Err(e) = std::fs::remove_dir(&path) {
        tracing::warn!(
            "SMP-T83 cgroup teardown: rmdir {} failed ({e}) — \
             likely non-empty (pids still alive?)",
            path.display()
        );
    }
}
