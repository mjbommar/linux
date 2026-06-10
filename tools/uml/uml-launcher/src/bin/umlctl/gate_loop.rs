// SPDX-License-Identifier: GPL-2.0
//
// `umlctl gate loop` — parallel up/wait/classify/stop/rm test loop.
//
// Replaces hand-rolled repeated-boot shell loops with a built-in
// flake-characterization harness:
//
//   1. Parallelism is one flag (`-W`) instead of `for w in ... &; wait`.
//   2. Per-iteration init.log capture happens BEFORE `umlctl rm` so
//      failure post-mortems aren't lost.
//   3. Classification uses the same regex grammar as `gate run`
//      (POSIX ERE via `grep -E`) — no Rust regex dep.
//   4. Wilson 95% CI is computed in-binary so you don't have to
//      eyeball "is 195/200 statistically the same as 957/1000?".
//   5. Sweeps (`--sweep KEY=v1,v2,...`) take the cartesian product
//      and run the loop per point.
//
// Per-worker isolation is by instance-name suffix: a Umlfile with
// `instance.name = "mt-mini"` and `--workers 4` becomes 4 instances
// `mt-mini-w0` .. `mt-mini-w3`, each running its own up/stop/rm
// cycle in a dedicated thread.

use anyhow::{bail, Context, Result};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::deploy;
use crate::paths;
use crate::supervise;

#[derive(Debug, Clone)]
struct LoopArgs {
    file: PathBuf,
    workers: u32,
    iters: u32,
    pass_marker: String,
    fail_marker: String,
    timeout_secs: u64,
    out_dir: PathBuf,
    sweep_axes: Vec<SweepAxis>,
    json: bool,
    kernel_override: Option<PathBuf>,
    network_driver_override: Option<String>,
    network_queues_override: Option<deploy::NetworkQueueSpec>,
    network_host_mode_override: Option<String>,
    strace: bool,
    audit_vector_sandbox: bool,
}

/// One `--sweep KEY=v1,v2,v3` axis.
#[derive(Debug, Clone)]
struct SweepAxis {
    key: String,
    values: Vec<String>,
}

/// One assignment from one cartesian-product point: KEY=value pairs
/// applied to every worker's Umlfile in this loop iteration.
#[derive(Debug, Clone)]
struct SweepPoint(Vec<(String, String)>);

impl SweepPoint {
    fn label(&self) -> String {
        if self.0.is_empty() {
            "default".to_string()
        } else {
            self.0
                .iter()
                .map(|(k, v)| format!("{k}={v}"))
                .collect::<Vec<_>>()
                .join(",")
        }
    }
}

#[derive(Debug, Clone)]
struct WorkerSpec {
    toml_path: PathBuf,
    strace_log: Option<PathBuf>,
}

#[derive(Debug)]
struct PointResult {
    label: String,
    pass: u32,
    fail: u32,
    timeout: u32,
    elapsed_secs: u64,
}

impl PointResult {
    fn total(&self) -> u32 {
        self.pass + self.fail + self.timeout
    }

    fn rate_pct(&self) -> f64 {
        let n = self.total();
        if n == 0 {
            return 0.0;
        }
        100.0 * (self.pass as f64) / (n as f64)
    }

    /// Wilson 95% CI for the PASS rate (z=1.96).
    /// Returns (low_pct, high_pct).
    fn wilson_ci_95(&self) -> (f64, f64) {
        let n = self.total() as f64;
        if n == 0.0 {
            return (0.0, 0.0);
        }
        let p = (self.pass as f64) / n;
        let z = 1.96_f64;
        let z2 = z * z;
        let denom = 1.0 + z2 / n;
        let centre = (p + z2 / (2.0 * n)) / denom;
        let half = z * ((p * (1.0 - p) / n) + z2 / (4.0 * n * n)).sqrt() / denom;
        (
            (100.0 * (centre - half)).max(0.0),
            (100.0 * (centre + half)).min(100.0),
        )
    }
}

pub(super) fn run(paths: &paths::Paths, args: super::GateLoopArgs, quiet: bool) -> Result<()> {
    let lo = parse_args(args)?;
    fs::create_dir_all(&lo.out_dir).with_context(|| format!("mkdir {}", lo.out_dir.display()))?;

    if lo.workers == 0 || lo.iters == 0 {
        bail!("workers and iters must both be > 0");
    }

    // Sanity-load the Umlfile so an obvious typo fails before we
    // spawn anything (mirrors `gate run` dry-run sanity).
    let _base = deploy::Umlfile::from_path(&lo.file)
        .with_context(|| format!("load Umlfile {}", lo.file.display()))?;

    let points = expand_sweep_matrix(&lo.sweep_axes);
    if !lo.json && !quiet {
        let total_per_point = lo.workers * lo.iters;
        eprintln!(
            "[umlctl gate loop] file={} W={} M={} N/point={} sweep_points={} timeout={}s out={}",
            lo.file.display(),
            lo.workers,
            lo.iters,
            total_per_point,
            points.len(),
            lo.timeout_secs,
            lo.out_dir.display(),
        );
    }

    let mut results = Vec::with_capacity(points.len());
    for (idx, point) in points.iter().enumerate() {
        let result = run_one_point(paths, &lo, point, idx, quiet)
            .with_context(|| format!("sweep point {}: {}", idx, point.label()))?;
        emit_result(&result, &lo, quiet);
        results.push(result);
    }

    // Final return code: nonzero if ANY sweep point had a fail or
    // timeout. Same convention as `gate run`'s exit-on-violation
    // behavior — makes this drop-in for CI.
    let any_failed = results.iter().any(|r| r.fail + r.timeout > 0);
    if any_failed {
        std::process::exit(1);
    }
    Ok(())
}

fn parse_args(args: super::GateLoopArgs) -> Result<LoopArgs> {
    let out_dir = args.out.unwrap_or_else(|| {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();
        PathBuf::from(format!("/tmp/umlctl-loop-{now}"))
    });

    let mut sweep_axes = Vec::with_capacity(args.sweep.len());
    for raw in &args.sweep {
        let (k, v) = raw
            .split_once('=')
            .with_context(|| format!("--sweep '{raw}': expected KEY=v1,v2,...; missing '='"))?;
        if k.is_empty() {
            bail!("--sweep '{raw}': empty key");
        }
        let values: Vec<String> = v
            .split(',')
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect();
        if values.is_empty() {
            bail!("--sweep '{raw}': no values");
        }
        sweep_axes.push(SweepAxis {
            key: k.to_string(),
            values,
        });
    }
    if let Some(driver) = args.network_driver.as_deref() {
        deploy::validate_network_driver(driver)
            .with_context(|| format!("validate --network-driver {driver}"))?;
        if sweep_axes.iter().any(|axis| axis.key == "network.driver") {
            bail!("use either --network-driver or --sweep network.driver=..., not both");
        }
    }
    if args.network_queues.is_some() {
        if sweep_axes.iter().any(|axis| axis.key == "network.queues") {
            bail!("use either --network-queues or --sweep network.queues=..., not both");
        }
    }
    if let Some(host_mode) = args.network_host_mode.as_deref() {
        if sweep_axes
            .iter()
            .any(|axis| axis.key == "network.host_mode")
        {
            bail!("use either --network-host-mode or --sweep network.host_mode=..., not both");
        }
        deploy::validate_network_host_mode(host_mode)
            .with_context(|| format!("validate --network-host-mode {host_mode}"))?;
    }

    Ok(LoopArgs {
        file: args.file,
        workers: args.workers,
        iters: args.iters,
        pass_marker: args.pass_marker,
        fail_marker: args.fail_marker,
        timeout_secs: args.timeout,
        out_dir,
        sweep_axes,
        json: args.json,
        kernel_override: args.kernel,
        network_driver_override: args.network_driver,
        network_queues_override: args.network_queues,
        network_host_mode_override: args.network_host_mode,
        strace: args.strace || args.audit_vector_sandbox,
        audit_vector_sandbox: args.audit_vector_sandbox,
    })
}

/// Expand axes into the cartesian product. An empty axis list yields
/// one default-flavor point so the loop runs at least once with no
/// env modifications.
fn expand_sweep_matrix(axes: &[SweepAxis]) -> Vec<SweepPoint> {
    let mut acc: Vec<SweepPoint> = vec![SweepPoint(Vec::new())];
    for axis in axes {
        let mut next = Vec::with_capacity(acc.len() * axis.values.len());
        for prev in &acc {
            for val in &axis.values {
                let mut combined = prev.0.clone();
                combined.push((axis.key.clone(), val.clone()));
                next.push(SweepPoint(combined));
            }
        }
        acc = next;
    }
    acc
}

fn run_one_point(
    paths: &paths::Paths,
    lo: &LoopArgs,
    point: &SweepPoint,
    point_idx: usize,
    quiet: bool,
) -> Result<PointResult> {
    let label = point.label();
    let point_subdir = lo
        .out_dir
        .join(format!("p{point_idx}_{}", sanitize(&label)));
    fs::create_dir_all(&point_subdir)?;

    let umlctl = std::env::current_exe().context("locate own exe")?;

    // Generate per-worker Umlfiles into the point subdir.
    let mut base = deploy::Umlfile::from_path(&lo.file)
        .with_context(|| format!("re-load Umlfile {}", lo.file.display()))?;
    if let Some(driver) = lo.network_driver_override.as_deref() {
        deploy::set_network_driver(&mut base, driver)
            .with_context(|| format!("apply --network-driver {driver}"))?;
    }
    if let Some(queues) = &lo.network_queues_override {
        deploy::set_network_queue_spec(&mut base, queues.clone())
            .with_context(|| format!("apply --network-queues {queues}"))?;
    }
    if let Some(host_mode) = lo.network_host_mode_override.as_deref() {
        deploy::set_network_host_mode(&mut base, host_mode)
            .with_context(|| format!("apply --network-host-mode {host_mode}"))?;
    }
    let stem = base.instance.name.clone();

    let mut worker_files: Vec<WorkerSpec> = Vec::with_capacity(lo.workers as usize);
    for w in 0..lo.workers {
        let mut u = base.clone();
        u.instance.name = format!("{stem}-w{w}");
        // base.debug.log_dir was already filled by Umlfile::from_path
        // with the ORIGINAL stem, so renaming instance.name without
        // also resetting log_dir would leave all workers writing the
        // SAME init.sh/strace paths concurrently. Keep generated
        // worker runtime files under the gate output directory so
        // audit artifacts stay with the copied per-iteration logs.
        let runtime_log_dir = point_subdir.join(format!("runtime-w{w}"));
        fs::create_dir_all(&runtime_log_dir)?;
        u.debug.log_dir = runtime_log_dir.display().to_string();
        if lo.strace {
            u.debug.strace = true;
        }
        apply_sweep_point(&mut u, point)?;
        // Optional kernel override (CLI > Umlfile).
        if let Some(k) = &lo.kernel_override {
            u.kernel.path = k.display().to_string();
        }
        let toml_path = point_subdir.join(format!("{stem}-w{w}.toml"));
        let serialized = toml::to_string_pretty(&u).context("serialize generated Umlfile")?;
        fs::write(&toml_path, serialized)
            .with_context(|| format!("write {}", toml_path.display()))?;
        worker_files.push(WorkerSpec {
            toml_path,
            strace_log: lo.strace.then(|| runtime_log_dir.join("strace.log")),
        });
    }

    let pass = Arc::new(AtomicU32::new(0));
    let fail = Arc::new(AtomicU32::new(0));
    let timeout = Arc::new(AtomicU32::new(0));

    let started = Instant::now();
    let mut handles = Vec::with_capacity(lo.workers as usize);
    for (w, worker) in worker_files.iter().enumerate() {
        let umlctl = umlctl.clone();
        let toml_path = worker.toml_path.clone();
        let strace_log = worker.strace_log.clone();
        let pass = Arc::clone(&pass);
        let fail = Arc::clone(&fail);
        let timeout = Arc::clone(&timeout);
        let inst_name = format!("{stem}-w{w}");
        let runs_dir = paths.runs_dir().to_path_buf();
        let run_id_path = paths.run_id_file_path(&inst_name);
        let pass_marker = lo.pass_marker.clone();
        let fail_marker = lo.fail_marker.clone();
        let timeout_secs = lo.timeout_secs;
        let iters = lo.iters;
        let audit_vector_sandbox = lo.audit_vector_sandbox;
        let log_dir = point_subdir.join(format!("w{w}"));
        fs::create_dir_all(&log_dir)?;
        let quiet = quiet || lo.json;

        handles.push(thread::spawn(move || {
            for i in 1..=iters {
                let status = run_one_iter(
                    &umlctl,
                    &toml_path,
                    &inst_name,
                    &runs_dir,
                    &run_id_path,
                    &pass_marker,
                    &fail_marker,
                    timeout_secs,
                    &log_dir,
                    strace_log.as_deref(),
                    audit_vector_sandbox,
                    i,
                );
                match status {
                    IterStatus::Pass => {
                        pass.fetch_add(1, Ordering::Relaxed);
                    }
                    IterStatus::Fail => {
                        fail.fetch_add(1, Ordering::Relaxed);
                    }
                    IterStatus::Timeout => {
                        timeout.fetch_add(1, Ordering::Relaxed);
                    }
                }
                if !quiet {
                    eprintln!("[umlctl gate loop] w{w} iter{i}: {status:?}");
                }
            }
        }));
    }
    for h in handles {
        let _ = h.join();
    }
    let elapsed_secs = started.elapsed().as_secs();

    Ok(PointResult {
        label,
        pass: pass.load(Ordering::Relaxed),
        fail: fail.load(Ordering::Relaxed),
        timeout: timeout.load(Ordering::Relaxed),
        elapsed_secs,
    })
}

fn apply_sweep_point(u: &mut deploy::Umlfile, point: &SweepPoint) -> Result<()> {
    // Most sweep keys are env vars; dotted config keys are reserved
    // for umlctl-owned convenience switches.
    for (k, v) in &point.0 {
        if k == "network.driver" {
            deploy::set_network_driver(u, v)
                .with_context(|| format!("apply --sweep network.driver={v}"))?;
        }
    }
    for (k, v) in &point.0 {
        if k == "network.queues" {
            let queues: deploy::NetworkQueueSpec = v
                .parse()
                .with_context(|| format!("parse --sweep network.queues={v}"))?;
            deploy::set_network_queue_spec(u, queues)
                .with_context(|| format!("apply --sweep network.queues={v}"))?;
        }
    }
    for (k, v) in &point.0 {
        if k == "network.host_mode" {
            deploy::set_network_host_mode(u, v)
                .with_context(|| format!("apply --sweep network.host_mode={v}"))?;
        }
    }
    for (k, v) in &point.0 {
        match k.as_str() {
            "network.driver" | "network.queues" | "network.host_mode" => {}
            _ => {
                u.env.insert(k.clone(), v.clone());
            }
        }
    }
    Ok(())
}

#[derive(Debug)]
enum IterStatus {
    Pass,
    Fail,
    Timeout,
}

#[allow(clippy::too_many_arguments)]
fn run_one_iter(
    umlctl: &Path,
    toml_path: &Path,
    inst_name: &str,
    runs_dir: &Path,
    run_id_path: &Path,
    pass_marker: &str,
    fail_marker: &str,
    timeout_secs: u64,
    log_dir: &Path,
    strace_log: Option<&Path>,
    audit_vector_sandbox: bool,
    iter: u32,
) -> IterStatus {
    // Make sure no leftover instance or host resources with this name
    // exist. `down` is important for TAP-backed Umlfiles because a
    // plain stop+rm skips host-side teardown.
    if !cleanup_umlfile_iter(umlctl, toml_path, inst_name, log_dir, iter, "pre") {
        return IterStatus::Fail;
    }

    // Spawn. We DON'T pass --wait-for here; we want full control
    // over the polling loop so that on timeout we can capture the
    // init.log before `stop` writes the kernel-shutdown banner.
    let up_log = log_dir.join(format!("up-{iter}.log"));
    let ready_timeout = timeout_secs.to_string();
    let up_out = run_umlctl(
        umlctl,
        &[
            "up",
            "-f",
            toml_path.to_str().unwrap(),
            "--ready-timeout",
            &ready_timeout,
        ],
    );
    let _ = fs::write(&up_log, &up_out);

    if !up_out.contains("started ") {
        // up failed before reaching the normal "started" line. Keep
        // the up output and, when umlctl surfaced one, the run bundle's
        // init.log before cleanup removes the failed instance.
        save_failed_up_diagnostics(&up_out, runs_dir, &log_dir.join(format!("run-{iter}.log")));
        cleanup_umlfile_iter(umlctl, toml_path, inst_name, log_dir, iter, "failed-up");
        return IterStatus::Fail;
    }

    // Poll the live run_id_file → init.log for either marker.
    //
    // PASS-wins classification: a successful run typically ends with
    // pass_marker followed by `Kernel panic - Attempted to kill init`
    // (UML's normal shutdown — init exiting causes the kernel to
    // panic). If we made fail_marker win we'd misclassify every clean
    // success as a failure. Real failures (mt-mini VERIFY_FAIL,
    // kernel BUG before test completion) won't have written the
    // pass_marker, so they fall through to the fail_marker check.
    let deadline = Instant::now() + Duration::from_secs(timeout_secs);
    let classified: IterStatus = loop {
        if let Some(run_id) = supervise::read_run_id_file(run_id_path) {
            let init_log = runs_dir.join(&run_id).join("init.log");
            if init_log.exists() {
                if grep_matches(pass_marker, &init_log) {
                    break IterStatus::Pass;
                }
                if grep_matches(fail_marker, &init_log) {
                    break IterStatus::Fail;
                }
            }
        }
        if Instant::now() >= deadline {
            break IterStatus::Timeout;
        }
        thread::sleep(Duration::from_millis(200));
    };

    // Capture the init.log INTO log_dir BEFORE stop+rm so a failure
    // post-mortem isn't lost when the bundle gets purged. This was
    // a real pain in the bash version of this loop — fixing it in
    // the binary so it can't be skipped.
    if let Some(run_id) = supervise::read_run_id_file(run_id_path) {
        let init_log = runs_dir.join(&run_id).join("init.log");
        let saved = log_dir.join(format!("run-{iter}.log"));
        copy_or_create(&init_log, &saved);
    }

    let cleanup_ok = cleanup_umlfile_iter(umlctl, toml_path, inst_name, log_dir, iter, "post");
    let strace_ok = capture_and_audit_strace(
        strace_log,
        audit_vector_sandbox,
        &log_dir.join(format!("strace-{iter}.log")),
        &log_dir.join(format!("strace-audit-{iter}.log")),
    );

    if !cleanup_ok || !strace_ok {
        return IterStatus::Fail;
    }

    classified
}

fn cleanup_umlfile_iter(
    umlctl: &Path,
    toml_path: &Path,
    inst_name: &str,
    log_dir: &Path,
    iter: u32,
    phase: &str,
) -> bool {
    let tap_name = cleanup_tap_name(toml_path);

    if let Some(toml) = toml_path.to_str() {
        let _ = run_umlctl(umlctl, &["down", "-f", toml, "--force", "--rm"]);
    } else {
        let _ = run_umlctl(umlctl, &["stop", inst_name]);
        let _ = run_umlctl(umlctl, &["rm", inst_name]);
    }

    audit_tap_absent(
        tap_name.as_deref(),
        Path::new("/sys/class/net"),
        &log_dir.join(format!("cleanup-{phase}-{iter}.log")),
    )
}

fn cleanup_tap_name(toml_path: &Path) -> Option<String> {
    let uml = deploy::Umlfile::from_path(toml_path).ok()?;

    (uml.network.mode == "tap").then_some(uml.network.tap_name)
}

fn audit_tap_absent(tap_name: Option<&str>, sys_class_net: &Path, dst: &Path) -> bool {
    let Some(tap) = tap_name else {
        return true;
    };

    if !tap_present_in(tap, sys_class_net) {
        return true;
    }

    let _ = fs::write(
        dst,
        format!(
            "cleanup audit failed: TAP device {tap} still exists under {}\n",
            sys_class_net.display()
        ),
    );
    false
}

fn tap_present_in(tap_name: &str, sys_class_net: &Path) -> bool {
    sys_class_net.join(tap_name).exists()
}

fn capture_and_audit_strace(
    strace_log: Option<&Path>,
    audit_vector_sandbox: bool,
    saved_trace: &Path,
    audit_log: &Path,
) -> bool {
    let Some(src) = strace_log else {
        return true;
    };

    if src.exists() {
        let _ = fs::copy(src, saved_trace);
    } else if audit_vector_sandbox {
        let _ = fs::write(
            audit_log,
            format!(
                "vector sandbox strace audit failed: missing {}\n",
                src.display()
            ),
        );
        return false;
    } else {
        return true;
    }

    if !audit_vector_sandbox {
        return true;
    }

    audit_vector_sandbox_strace(src, audit_log)
}

fn audit_vector_sandbox_strace(strace_log: &Path, audit_log: &Path) -> bool {
    let s = match fs::read_to_string(strace_log) {
        Ok(s) => s,
        Err(e) => {
            let _ = fs::write(
                audit_log,
                format!(
                    "vector sandbox strace audit failed: read {}: {e}\n",
                    strace_log.display()
                ),
            );
            return false;
        }
    };

    for (idx, line) in s.lines().enumerate() {
        if let Some(reason) = forbidden_vector_sandbox_line(line) {
            let _ = fs::write(
                audit_log,
                format!(
                    "vector sandbox strace audit failed: {reason} at line {}\n{}\n",
                    idx + 1,
                    line
                ),
            );
            return false;
        }
    }

    let _ = fs::write(
        audit_log,
        "vector sandbox strace audit passed: no host TAP open, TUNSETIFF, AF_PACKET, bpf(), or UML network-helper exec\n",
    );
    true
}

fn forbidden_vector_sandbox_line(line: &str) -> Option<&'static str> {
    let (pid, rest) = line.split_once(' ')?;
    if pid.is_empty() || !pid.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }

    if (rest.starts_with("open(") || rest.starts_with("openat("))
        && rest.contains("\"/dev/net/tun\"")
    {
        return Some("/dev/net/tun open");
    }
    if rest.starts_with("ioctl(") && rest.contains("TUNSETIFF") {
        return Some("TUNSETIFF ioctl");
    }
    if rest.starts_with("socket(AF_PACKET") {
        return Some("AF_PACKET socket");
    }
    if rest.starts_with("bpf(") {
        return Some("bpf syscall");
    }
    if (rest.starts_with("execve(") || rest.starts_with("execveat("))
        && (rest.contains("uml_net") || rest.contains("uml_switch"))
    {
        return Some("UML network helper exec");
    }

    None
}

fn run_umlctl(umlctl: &Path, args: &[&str]) -> String {
    Command::new(umlctl)
        .args(args)
        .output()
        .map(|o| {
            let mut s = String::from_utf8_lossy(&o.stdout).into_owned();
            s.push_str(&String::from_utf8_lossy(&o.stderr));
            s
        })
        .unwrap_or_default()
}

fn grep_matches(pattern: &str, file: &Path) -> bool {
    let Some(file_s) = file.to_str() else {
        return false;
    };
    Command::new("grep")
        .args(["-E", "-q", pattern, file_s])
        .status()
        .map(|st| st.success())
        .unwrap_or(false)
}

fn copy_or_create(src: &Path, dst: &Path) {
    if src.exists() {
        let _ = fs::copy(src, dst);
    } else {
        let _ = fs::write(dst, "(no init.log)\n");
    }
}

fn save_failed_up_diagnostics(up_out: &str, runs_dir: &Path, dst: &Path) {
    let mut out = String::from("== umlctl up output ==\n");
    out.push_str(up_out);
    if !out.ends_with('\n') {
        out.push('\n');
    }

    if let Some(init_log) = failed_up_init_log_path(up_out, runs_dir) {
        out.push_str(&format!(
            "\n== captured init.log ({}) ==\n",
            init_log.display()
        ));
        match fs::read_to_string(&init_log) {
            Ok(contents) => {
                out.push_str(&contents);
                if !out.ends_with('\n') {
                    out.push('\n');
                }
            }
            Err(e) => {
                out.push_str(&format!("(could not read init.log: {e})\n"));
            }
        }
    } else {
        out.push_str("\n(no run_id/init_log in umlctl up output)\n");
    }

    let _ = fs::write(dst, out);
}

fn failed_up_init_log_path(up_out: &str, runs_dir: &Path) -> Option<PathBuf> {
    if let Some(path) = output_field(up_out, "init_log") {
        return Some(PathBuf::from(path));
    }
    output_field(up_out, "run_id").map(|run_id| runs_dir.join(run_id).join("init.log"))
}

fn output_field(output: &str, field: &str) -> Option<String> {
    let prefix = format!("{field}=");
    output
        .split_whitespace()
        .find_map(|token| token.strip_prefix(&prefix))
        .map(|value| value.trim_matches(|c| c == ',' || c == ';').to_string())
        .filter(|value| !value.is_empty() && value != "-")
}

/// Scrub a sweep label down to filesystem-safe characters.
fn sanitize(s: &str) -> String {
    s.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' {
                c
            } else {
                '_'
            }
        })
        .collect()
}

fn emit_result(r: &PointResult, lo: &LoopArgs, quiet: bool) {
    if lo.json {
        let (lo_pct, hi_pct) = r.wilson_ci_95();
        let mut sweep = serde_json::Map::new();
        if r.label != "default" {
            for kv in r.label.split(',') {
                if let Some((k, v)) = kv.split_once('=') {
                    sweep.insert(k.to_string(), serde_json::Value::String(v.to_string()));
                }
            }
        }
        let row = serde_json::json!({
            "schema": "umlctl.gate.loop.v1",
            "sweep": sweep,
            "pass": r.pass,
            "fail": r.fail,
            "timeout": r.timeout,
            "n": r.total(),
            "rate_pct": r.rate_pct(),
            "ci95_lo_pct": lo_pct,
            "ci95_hi_pct": hi_pct,
            "elapsed_secs": r.elapsed_secs,
        });
        // NDJSON: one row per line on stdout.
        let _ = writeln!(std::io::stdout(), "{row}");
    } else if !quiet {
        let (lo_pct, hi_pct) = r.wilson_ci_95();
        println!(
            "==> {} PASS={}/{} FAIL={} TIMEOUT={} rate={:.1}% (Wilson 95% CI [{:.1}%, {:.1}%]) elapsed={}s",
            r.label,
            r.pass,
            r.total(),
            r.fail,
            r.timeout,
            r.rate_pct(),
            lo_pct,
            hi_pct,
            r.elapsed_secs,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wilson_ci_95_known_values() {
        // PASS=957 FAIL=43, n=1000: rate=95.7%, Wilson CI roughly
        // 94.2%..96.9%.
        let r = PointResult {
            label: "x".into(),
            pass: 957,
            fail: 43,
            timeout: 0,
            elapsed_secs: 0,
        };
        let (lo, hi) = r.wilson_ci_95();
        assert!((lo - 94.1).abs() < 0.5, "lo={lo}");
        assert!((hi - 96.9).abs() < 0.5, "hi={hi}");
    }

    #[test]
    fn sweep_matrix_cartesian() {
        let axes = vec![
            SweepAxis {
                key: "A".into(),
                values: vec!["1".into(), "2".into()],
            },
            SweepAxis {
                key: "B".into(),
                values: vec!["x".into(), "y".into()],
            },
        ];
        let pts = expand_sweep_matrix(&axes);
        assert_eq!(pts.len(), 4);
        let labels: Vec<String> = pts.iter().map(|p| p.label()).collect();
        assert!(labels.contains(&"A=1,B=x".to_string()));
        assert!(labels.contains(&"A=2,B=y".to_string()));
    }

    #[test]
    fn sweep_matrix_empty_yields_default() {
        let pts = expand_sweep_matrix(&[]);
        assert_eq!(pts.len(), 1);
        assert_eq!(pts[0].label(), "default");
    }

    #[test]
    fn network_driver_sweep_updates_config_not_env() {
        let mut u: deploy::Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
"#,
        )
        .unwrap();
        let point = SweepPoint(vec![
            ("network.queues".into(), "2".into()),
            ("network.driver".into(), "vector2".into()),
            ("network.host_mode".into(), "inproc".into()),
            ("MT_JITTER_NS".into(), "100".into()),
        ]);

        apply_sweep_point(&mut u, &point).unwrap();

        assert_eq!(u.network.driver, "vector2");
        assert_eq!(u.network.queues, deploy::NetworkQueueSpec::Fixed(2));
        assert_eq!(u.network.host_mode, "inproc");
        assert_eq!(u.env.get("MT_JITTER_NS").map(String::as_str), Some("100"));
        assert!(!u.env.contains_key("network.driver"));
        assert!(!u.env.contains_key("network.queues"));
        assert!(!u.env.contains_key("network.host_mode"));
    }

    #[test]
    fn network_queue_sweep_accepts_auto_spec() {
        let mut u: deploy::Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[runtime]
ncpus = 4
[network]
mode = "tap"
driver = "vector2"
"#,
        )
        .unwrap();
        let point = SweepPoint(vec![("network.queues".into(), "auto".into())]);

        apply_sweep_point(&mut u, &point).unwrap();

        assert_eq!(u.network.queues, deploy::NetworkQueueSpec::Auto);
        assert_eq!(
            deploy::resolve_network_queues(&u.network, &u.runtime).unwrap(),
            4
        );
        assert!(!u.env.contains_key("network.queues"));
    }

    #[test]
    fn tap_cleanup_audit_passes_when_tap_absent() {
        let sys = tempfile::tempdir().unwrap();
        let out = sys.path().join("cleanup.log");

        assert!(audit_tap_absent(Some("v2fd0"), sys.path(), &out));
        assert!(!out.exists());
    }

    #[test]
    fn tap_cleanup_audit_fails_when_tap_remains() {
        let sys = tempfile::tempdir().unwrap();
        let out = sys.path().join("cleanup.log");
        fs::create_dir(sys.path().join("v2fd0")).unwrap();

        assert!(!audit_tap_absent(Some("v2fd0"), sys.path(), &out));
        let log = fs::read_to_string(out).unwrap();
        assert!(log.contains("cleanup audit failed"));
        assert!(log.contains("v2fd0"));
    }

    #[test]
    fn failed_up_diagnostics_prefers_init_log_field() {
        let runs = Path::new("/state/runs");
        let out = "umlctl: instance 'x' did not become ready within 180s pid=42 run_id=01ABC init_log=/tmp/run/init.log\n";

        assert_eq!(
            failed_up_init_log_path(out, runs).unwrap(),
            PathBuf::from("/tmp/run/init.log")
        );
    }

    #[test]
    fn failed_up_diagnostics_falls_back_to_run_id() {
        let runs = Path::new("/state/runs");
        let out = "umlctl: instance 'x' did not become ready within 180s pid=42 run_id=01ABC init_log=-\n";

        assert_eq!(
            failed_up_init_log_path(out, runs).unwrap(),
            PathBuf::from("/state/runs/01ABC/init.log")
        );
    }

    #[test]
    fn vector_sandbox_audit_ignores_string_buffers_and_guest_netlink() {
        assert_eq!(
            forbidden_vector_sandbox_line(
                "12 pread64(17, \"\\0/dev/net/tun\\0ioctl(TUNSETIFF)\"..., 4096, 0) = 4096"
            ),
            None
        );
        assert_eq!(
            forbidden_vector_sandbox_line(
                "13 socket(AF_NETLINK, SOCK_RAW|SOCK_CLOEXEC, NETLINK_ROUTE) = 41"
            ),
            None
        );
    }

    #[test]
    fn vector_sandbox_audit_rejects_host_operations() {
        assert_eq!(
            forbidden_vector_sandbox_line(
                "12 openat(AT_FDCWD, \"/dev/net/tun\", O_RDWR|O_CLOEXEC) = 4"
            ),
            Some("/dev/net/tun open")
        );
        assert_eq!(
            forbidden_vector_sandbox_line("12 ioctl(4, TUNSETIFF, 0x7ffc) = 0"),
            Some("TUNSETIFF ioctl")
        );
        assert_eq!(
            forbidden_vector_sandbox_line("12 socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)) = 4"),
            Some("AF_PACKET socket")
        );
        assert_eq!(
            forbidden_vector_sandbox_line("12 bpf(BPF_PROG_LOAD, 0x7ffc, 144) = 4"),
            Some("bpf syscall")
        );
        assert_eq!(
            forbidden_vector_sandbox_line(
                "12 execve(\"/usr/lib/uml/uml_net\", [\"uml_net\"], 0x7ffc) = 0"
            ),
            Some("UML network helper exec")
        );
    }
}
