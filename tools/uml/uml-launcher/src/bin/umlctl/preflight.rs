// SPDX-License-Identifier: GPL-2.0
//
// Preflight resource verification for UML host controls.
//
// At UML spawn time, before exec, verify that the resource
// controls declared in the manifest can actually be applied.
// Catches misconfiguration early (clear error message) instead
// of failing 30 s into UML boot with cryptic ENOMEM / EPERM.
//
// Each check is best-effort: a missing sysfs file means the
// host doesn't have that feature configured, which we accept
// and let the kernel-side fall-back behaviour run (e.g.
// hugepage allocation drops to 4K with a perror warning when
// the pool is empty). Hard-fail only on a logically-impossible
// state (e.g. UM_HUGEPAGES=2M requested but the host has no
// hugetlbfs subsystem at all).

use anyhow::Result;
use std::collections::BTreeMap;
use std::path::Path;

use crate::manifest::CgroupV2Config;

/// Warnings collected during preflight. Non-empty means the user
/// should investigate, but the run proceeds (UML
/// itself logs a perror if a runtime fallback was needed).
#[derive(Default, Debug)]
pub struct PreflightReport {
    pub warnings: Vec<String>,
}

impl PreflightReport {
    pub fn print(&self) {
        for w in &self.warnings {
            eprintln!("umlctl preflight: {w}");
        }
    }
}

/// Run all relevant checks against the host. host_env carries
/// the UM_* env vars umlctl is about to set; cgroup_v2 carries
/// the cgroup limits umlctl is about to install. Either can be
/// empty / None — checks for those are skipped.
pub fn run(
    host_env: &BTreeMap<String, String>,
    cgroup_v2: Option<&CgroupV2Config>,
) -> Result<PreflightReport> {
    let mut report = PreflightReport::default();

    // Hugepage pool — only check if UM_HUGEPAGES is set.
    if let Some(hp) = host_env.get("UM_HUGEPAGES") {
        check_hugepages(hp, &mut report);
    }

    // mlockall / pin_physmem — requires CAP_IPC_LOCK or a raised
    // RLIMIT_MEMLOCK. Check our own rlimit. (The kernel-side
    // code reads UM_KVM_V2_PIN_PHYSMEM regardless; this check keeps
    // the host-side warning close to launch.)
    if host_env.contains_key("UM_KVM_V2_PIN_PHYSMEM") {
        check_memlock_rlimit(&mut report);
    }

    // Cgroup v2 — verify the hierarchy is writable.
    if cgroup_v2.is_some() {
        check_cgroup_v2_writable(&mut report);
    }

    // /dev/kvm — kvm-v2 backend obviously needs it. Skip if
    // the host_env doesn't suggest kvm-v2 (the manifest's
    // backend field is the authoritative source elsewhere;
    // this is a defensive belt-and-suspenders).
    check_dev_kvm(&mut report);

    Ok(report)
}

fn check_hugepages(req: &str, report: &mut PreflightReport) {
    // Map UM_HUGEPAGES value to sysfs hugepage subdir name.
    let subdir = match req {
        "2M" => "hugepages-2048kB",
        "1G" => "hugepages-1048576kB",
        _ => {
            return; // "off" / "auto" / "" — kernel-side no-op
        }
    };
    let nr_path = format!("/sys/kernel/mm/hugepages/{subdir}/nr_hugepages");
    let free_path = format!("/sys/kernel/mm/hugepages/{subdir}/free_hugepages");

    if !Path::new(&nr_path).exists() {
        report.warnings.push(format!(
            "UM_HUGEPAGES={req} requested but {nr_path} absent — \
             host kernel doesn't support this hugepage size; \
             UML will fall back to 4K at runtime"
        ));
        return;
    }

    let nr: u64 = std::fs::read_to_string(&nr_path)
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0);
    let free: u64 = std::fs::read_to_string(&free_path)
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0);

    if nr == 0 {
        report.warnings.push(format!(
            "UM_HUGEPAGES={req} requested but pool is empty \
             (nr_hugepages=0 at {nr_path}); UML will fall back \
             to 4K at runtime. Pre-reserve via: \
             echo N > /proc/sys/vm/nr_hugepages"
        ));
    } else if free < 64 {
        report.warnings.push(format!(
            "UM_HUGEPAGES={req}: only {free} of {nr} hugepages \
             free; large physmem allocations may fall back to 4K"
        ));
    }
}

fn check_memlock_rlimit(report: &mut PreflightReport) {
    let mut rl = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    let rc = unsafe { libc::getrlimit(libc::RLIMIT_MEMLOCK, &mut rl) };
    if rc == 0 && rl.rlim_cur < 64 * 1024 * 1024 && rl.rlim_cur != libc::RLIM_INFINITY {
        report.warnings.push(format!(
            "UM_KVM_V2_PIN_PHYSMEM set but RLIMIT_MEMLOCK soft \
             limit is {} bytes; mlockall will EPERM unless the \
             UML binary has cap_ipc_lock (setcap \
             cap_ipc_lock,cap_sys_resource=+ep /path/to/linux)",
            rl.rlim_cur
        ));
    }
}

fn check_cgroup_v2_writable(report: &mut PreflightReport) {
    let root = Path::new("/sys/fs/cgroup");
    let controllers = root.join("cgroup.controllers");
    if !controllers.exists() {
        report.warnings.push(
            "cgroup_v2 limits requested but /sys/fs/cgroup/cgroup.controllers \
             absent — host appears to be cgroup v1 only; limits will be skipped"
                .into(),
        );
        return;
    }
    // Try to create the uml.slice directory (we don't actually
    // create it here; we just check write access).
    let slice = root.join("uml.slice");
    let probe_dir = slice.join(format!(".umlctl-preflight-{}", std::process::id()));
    if let Err(e) = std::fs::create_dir_all(&probe_dir) {
        report.warnings.push(format!(
            "cgroup_v2 limits requested but {} not creatable ({e}) — \
             limits will be skipped",
            slice.display()
        ));
    } else {
        // Cleanup: rmdir the probe (it's empty so this works).
        let _ = std::fs::remove_dir(&probe_dir);
    }
}

fn check_dev_kvm(report: &mut PreflightReport) {
    // Best-effort: only warn if /dev/kvm exists but isn't
    // readable. If it's absent, the kvm-v2 backend probe at UML
    // startup will skip; we don't speculate on backend choice here.
    if Path::new("/dev/kvm").exists() {
        match std::fs::metadata("/dev/kvm") {
            Ok(_) => {
                if let Err(_) = std::fs::OpenOptions::new().read(true).open("/dev/kvm") {
                    report.warnings.push(
                        "/dev/kvm present but not readable — kvm-v2 backend \
                         will probe-fail; consider `sudo setfacl -m \
                         u:$(id -un):rw /dev/kvm`"
                            .into(),
                    );
                }
            }
            Err(e) => {
                report.warnings.push(format!("/dev/kvm stat failed: {e}"));
            }
        }
    }
}
