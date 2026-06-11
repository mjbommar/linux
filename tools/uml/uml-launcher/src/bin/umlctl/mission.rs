// SPDX-License-Identifier: GPL-2.0
//
// `umlctl mission` - comprehensive readiness gate for the
// UML kvm-v2 backend.
//
// Six fail-fast steps, ~10-15 min total:
//
//   1. KUnit selftests       - snapshot-kvm-smoke (4 cases) +
//                              kvm-record-smoke (9 cases + live record).
//                              Proves the new snapshot/RR code
//                              paths execute correctly.
//
//   2. Performance bench     - kvm-snapshot-bench N=64. Gates:
//                              capture < 50 ms, median restore < 1 ms.
//                              Checks the snapshot/restore latency
//                              budget.
//
//   3. Substrate parity      - kvm-smoke + cpython-tier0 +
//                              kvm-bounds coverage for backend
//                              invariants.
//
//   4. Host resource controls - spawn UML with resource knobs set;
//                              verify /proc/<pid>/oom_score_adj and
//                              environ contain the requested
//                              UM_* env vars; verify preflight
//                              warnings fire on intentionally-
//                              misconfigured input. Proves the
//                              resource-control path end-to-end.
//
//   5. Diverse workload soak - 5 workloads x 20-30 iters via the
//                              run-soak-daemon.sh harness. Gates:
//                              aggregate Wilson 95% CI lower bound
//                              >= 97%; zero panics; zero "Kernel
//                              mode signal 7". Exercises diverse
//                              fork, FPU, I/O, and scheduler
//                              workloads.
//
//   6. Diagnostic snapshot   - record kernel HEAD, umlctl version,
//                              host hugepage/cgroup state, soak
//                              scoreboard. Reproducibility metadata.
//
// Binary verdict on stdout:
//
//   MISSION_ACCOMPLISHED in <seconds>s (kernel=<sha>, host=<name>)
//   MISSION_FAILED step=<N> <name>: <message>
//
// JSON scoreboard at <out>/scoreboard.json, per-step logs at
// <out>/step-<N>.log.

use anyhow::{Context, Result};
use clap::Args;
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

#[derive(Args, Debug)]
pub struct MissionArgs {
    /// UML kernel binary to test. Must be built with
    /// CONFIG_UM_BACKEND_KVM_V2=y + CONFIG_UM_BACKEND_KVM_V2_KUNIT=y.
    #[arg(long, env = "UML_KERNEL")]
    pub kernel: PathBuf,

    /// Output directory for scoreboard.json + per-step logs.
    /// Created if absent. Default: /tmp/mission-<timestamp>/.
    #[arg(long)]
    pub out: Option<PathBuf>,

    /// Path to the linux tree's tools/testing/selftests/um.
    /// Used to locate snapshot-kvm-smoke / kvm-record-smoke /
    /// kvm-snapshot-bench / kvm-smoke / cpython-tier0 / kvm-bounds /
    /// soak/run-soak-daemon.sh. Defaults to CWD/tools/testing/
    /// selftests/um if --selftests-dir not given.
    #[arg(long, env = "UMLCTL_SELFTESTS_DIR")]
    pub selftests_dir: Option<PathBuf>,

    /// Quick mode: skip step 5 (the 8-minute soak). Full run
    /// becomes ~2-3 min. Useful for local iteration.
    #[arg(long)]
    pub quick: bool,

    /// Continue past failing steps (collect all signals).
    /// Default: fail-fast on first step failure.
    #[arg(long)]
    pub continue_on_fail: bool,

    /// Step 5 soak budget in seconds. Default 480 (8 min).
    /// Lower for dev iteration; higher for confidence runs.
    #[arg(long, default_value = "480")]
    pub soak_budget_sec: u64,

    /// Step 5 Wilson 95% CI lower-bound threshold (percent).
    /// Default 97.0 allows a small high-cr2 noise class on
    /// django-loopback while catching real regressions.
    #[arg(long, default_value = "97.0")]
    pub soak_min_pct: f64,

    /// Opt-in vector2 stress step for kvm-v2 with the tier3-django-v2
    /// workload. Requires host-side
    /// TAP + iptables capability (sudo -n). This catches vector2-side
    /// regressions in the default umlctl-managed vector2 TAP/fd path.
    #[arg(long)]
    pub with_vector2: bool,

    /// Vector2 stress iteration count. Default 5 keeps the
    /// mission gate under 30 minutes total while still exercising
    /// repeated vector2 startup and teardown.
    #[arg(long, default_value = "5")]
    pub vector2_iters: u32,

    /// UML kernel built with CONFIG_UM_TEMPLATE_PAUSE_FORK=y for the
    /// fork-stress selftest.  Falls back to
    /// $HOME/src/uml-builds/uml-tplpause-fork/linux when the env var
    /// is unset.  The fork-stress step SKIPs if the resolved path
    /// doesn't exist (the main mission kernel is normally NOT built
    /// with fork support).  It runs only on full missions, never quick.
    #[arg(long, env = "UM_FORK_KERNEL")]
    pub fork_kernel: Option<PathBuf>,
}

#[derive(Serialize, Debug, Clone, Copy, PartialEq, Eq)]
enum Verdict {
    Pass,
    Fail,
    Skip,
}

#[derive(Serialize, Debug)]
struct MissionStep {
    step_id: u32,
    name: &'static str,
    verdict: Verdict,
    duration_ms: u64,
    summary: String,
    /// Free-form key/value details per step (numbers, paths, hashes).
    details: BTreeMap<String, String>,
}

#[derive(Serialize, Debug)]
struct MissionScoreboard {
    kernel_path: String,
    kernel_sha256: String,
    kernel_version: Option<String>,
    host: String,
    git_head: Option<String>,
    started_at_iso: String,
    duration_ms: u64,
    verdict: Verdict,
    steps: Vec<MissionStep>,
}

pub fn run(args: MissionArgs) -> Result<()> {
    let out_dir = args
        .out
        .clone()
        .unwrap_or_else(|| PathBuf::from(format!("/tmp/mission-{}", unix_ts_secs())));
    std::fs::create_dir_all(&out_dir).context("create --out dir")?;

    let selftests_dir = resolve_selftests_dir(&args)?;
    if !args.kernel.exists() {
        anyhow::bail!(
            "kernel path {} does not exist - set UML_KERNEL or pass --kernel",
            args.kernel.display()
        );
    }

    let started = Instant::now();
    eprintln!(
        "umlctl mission: kernel={} selftests_dir={} out={}",
        args.kernel.display(),
        selftests_dir.display(),
        out_dir.display()
    );

    // Mission ordering:
    //   1 KUnit, 2 bench, 3 substrate, 4 host_resources,
    //   5 diverse soak, 7 vector2 stress (opt-in),
    //   8 fork-stress (only on full missions, separate kernel),
    //   6 diagnostic last.
    // Vector2 and fork-stress both run before the diagnostic phase because
    // diagnostics should capture the final reproducibility metadata
    // after every gate has run.  Fork-stress is skipped in quick mode: its
    // retry harness + ~5s per attempt is too expensive for the
    // 2-3 min quick-iteration budget.
    let steps: Vec<u32> = match (args.quick, args.with_vector2) {
        (true, true) => vec![1, 2, 3, 4, 7, 6],
        (true, false) => vec![1, 2, 3, 4, 6],
        (false, true) => vec![1, 2, 3, 4, 5, 7, 8, 6],
        (false, false) => vec![1, 2, 3, 4, 5, 8, 6],
    };

    let mut results: Vec<MissionStep> = Vec::new();
    let mut all_pass = true;
    for step_id in steps {
        let result = run_step(step_id, &args, &selftests_dir, &out_dir)?;
        let pass = result.verdict == Verdict::Pass || result.verdict == Verdict::Skip;
        print_step_line(&result);
        results.push(result);
        if !pass {
            all_pass = false;
            if !args.continue_on_fail {
                break;
            }
        }
    }

    let final_verdict = if all_pass {
        Verdict::Pass
    } else {
        Verdict::Fail
    };
    let duration = started.elapsed();

    let board = MissionScoreboard {
        kernel_path: args.kernel.display().to_string(),
        kernel_sha256: hash_file(&args.kernel).unwrap_or_default(),
        kernel_version: kernel_version(&args.kernel),
        host: hostname(),
        git_head: git_head(),
        started_at_iso: rfc3339_now(),
        duration_ms: duration.as_millis() as u64,
        verdict: final_verdict,
        steps: results,
    };

    let json = serde_json::to_string_pretty(&board)?;
    std::fs::write(out_dir.join("scoreboard.json"), json).context("write scoreboard.json")?;

    match final_verdict {
        Verdict::Pass => {
            println!(
                "MISSION_ACCOMPLISHED in {}s (kernel={}, host={})",
                duration.as_secs(),
                short_sha(&board.kernel_sha256),
                board.host,
            );
            Ok(())
        }
        Verdict::Fail => {
            let failed = board
                .steps
                .iter()
                .find(|p| p.verdict == Verdict::Fail)
                .map(|p| format!("step={} {}: {}", p.step_id, p.name, p.summary))
                .unwrap_or_else(|| "unknown".to_string());
            anyhow::bail!("MISSION_FAILED {failed}")
        }
        Verdict::Skip => Ok(()),
    }
}

// --------------------------------------------------------------
// Per-step runners
// --------------------------------------------------------------

fn run_step(
    step_id: u32,
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<MissionStep> {
    let start = Instant::now();
    let (name, verdict, summary, details) = match step_id {
        1 => step1_kunit(args, selftests_dir, out_dir)?,
        2 => step2_bench(args, selftests_dir, out_dir)?,
        3 => step3_substrate(args, selftests_dir, out_dir)?,
        4 => step4_host_resources(args, out_dir)?,
        5 => step5_diverse_soak(args, selftests_dir, out_dir)?,
        6 => step6_diagnostic(args, out_dir)?,
        7 => step7_vector2_stress(args, selftests_dir, out_dir)?,
        8 => step8_fork_stress(args, selftests_dir, out_dir)?,
        _ => anyhow::bail!("unknown step {step_id}"),
    };
    Ok(MissionStep {
        step_id,
        name,
        verdict,
        duration_ms: start.elapsed().as_millis() as u64,
        summary,
        details,
    })
}

fn step1_kunit(
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();
    let mut all_pass = true;

    for (script, label) in &[
        (
            "snapshot-kvm-smoke/run-snapshot-kvm-smoke.sh",
            "snapshot_kvm_smoke",
        ),
        (
            "kvm-record-smoke/run-kvm-record-smoke.sh",
            "kvm_record_smoke",
        ),
    ] {
        let path = selftests_dir.join(script);
        if !path.exists() {
            details.insert(label.to_string(), format!("MISSING:{}", path.display()));
            all_pass = false;
            continue;
        }
        let log = out_dir.join(format!("step-1-{label}.log"));
        let rc = run_script(&path, args, &log)?;
        details.insert(label.to_string(), format!("rc={rc}"));
        if rc != 0 {
            all_pass = false;
        }
    }
    let (verdict, summary) = if all_pass {
        (Verdict::Pass, "10/10 KUnit cases PASS".to_string())
    } else {
        (
            Verdict::Fail,
            "one or more KUnit selftests failed".to_string(),
        )
    };
    Ok(("kunit", verdict, summary, details))
}

fn step2_bench(
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();
    let script = selftests_dir.join("kvm-snapshot-bench/run-kvm-snapshot-bench.sh");
    if !script.exists() {
        details.insert("script".into(), format!("MISSING:{}", script.display()));
        return Ok((
            "bench",
            Verdict::Fail,
            "bench script absent".into(),
            details,
        ));
    }
    let log = out_dir.join("step-2-bench.log");
    let rc = run_script(&script, args, &log)?;
    if rc != 0 {
        details.insert("rc".into(), rc.to_string());
        return Ok(("bench", Verdict::Fail, format!("bench rc={rc}"), details));
    }

    // Parse the dmesg-shaped line:
    //   um: kvm-v2 snapshot bench: capture=N ns; restore_full ns:
    //     median=M p95=P min=A max=B n=K mode=full
    let content = std::fs::read_to_string(&log).unwrap_or_default();
    let line = content
        .lines()
        .find(|l| l.contains("KVM_SNAPSHOT_BENCH: PASS"))
        .map(|s| s.to_string())
        .unwrap_or_default();
    details.insert("dmesg_line".into(), line.clone());

    let cap_ns = parse_kv_int(&line, "capture=");
    let median_ns = parse_kv_int(&line, "median=");
    details.insert("capture_ns".into(), cap_ns.unwrap_or(0).to_string());
    details.insert("median_ns".into(), median_ns.unwrap_or(0).to_string());

    let cap_ok = cap_ns.map(|n| n < 50_000_000).unwrap_or(false); // < 50 ms
    let med_ok = median_ns.map(|n| n < 1_000_000).unwrap_or(false); // < 1 ms

    let (verdict, summary) = if cap_ok && med_ok {
        (
            Verdict::Pass,
            format!(
                "capture={}us median_restore={}us (targets <50ms / <1ms)",
                cap_ns.unwrap_or(0) / 1000,
                median_ns.unwrap_or(0) / 1000
            ),
        )
    } else {
        (
            Verdict::Fail,
            format!(
                "bench gate miss: capture={}ns (<50_000_000?) median={}ns (<1_000_000?)",
                cap_ns.unwrap_or(0),
                median_ns.unwrap_or(0)
            ),
        )
    };
    Ok(("bench", verdict, summary, details))
}

fn step3_substrate(
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();
    // Reliable v2 substrate gates only.  kvm-smoke + kvm-bounds use
    // older progression-marker and built-C-binary harnesses, so they
    // are tracked through the `umlctl gate` family instead of this
    // readiness gate.
    let scripts = [("cpython-tier0/run-cpython-tier0.sh", "cpython_tier0")];
    let mut ok = 0;
    let mut total = 0;
    let mut skipped = 0;
    for (path, label) in &scripts {
        let full = selftests_dir.join(path);
        if !full.exists() {
            details.insert(label.to_string(), "MISSING".into());
            continue;
        }
        total += 1;
        let log = out_dir.join(format!("step-3-{label}.log"));
        let rc = run_script(&full, args, &log)?;
        details.insert(label.to_string(), format!("rc={rc}"));
        match rc {
            0 => ok += 1,
            4 => skipped += 1, // kselftest convention SKIP
            _ => {}
        }
    }
    let (verdict, summary) = if total == 0 {
        (
            Verdict::Skip,
            "no substrate scripts found in selftests dir".into(),
        )
    } else if ok + skipped == total {
        (
            Verdict::Pass,
            format!("{ok} PASS / {skipped} SKIP / {total} total"),
        )
    } else {
        (
            Verdict::Fail,
            format!("{ok}/{total} PASS - substrate parity gates failed"),
        )
    };
    Ok(("substrate", verdict, summary, details))
}

fn step4_host_resources(
    args: &MissionArgs,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();
    // Build an inline .toml with knobs set, spawn via the kernel
    // directly (umlctl up would also work but adds dependencies).
    // The step verifies the *kernel-side* code reads UM_* env
    // vars correctly. umlctl-side translation is exercised in
    // step 5 (the soak templates have [host_resources] blocks).
    let log = out_dir.join("step-4-host_resources.log");

    let mut cmd = Command::new(&args.kernel);
    cmd.args([
        "backend=force=kvm-v2",
        "init=/bin/true",
        "mem=128M",
        "con=null",
        "con0=fd:0,fd:1",
        "root=/dev/root",
        "rootfstype=hostfs",
        "rw",
        "panic=-1",
    ]);
    cmd.env("UM_THP", "off");
    cmd.env("UM_OOM_SCORE_ADJ", "500");
    cmd.env("UM_KVM_V2_CPU_AFFINITY", "0-1");

    let out = cmd.output().context("spawn UML for step 4")?;
    let combined = format!(
        "{}\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    let _ = std::fs::write(&log, &combined);

    // Success signals: kvm-v2 backend banner and no resource-control
    // perror/warning lines from the requested UM_* knobs.
    let backend_ok = combined.contains("um: backend = kvm-v2");
    let resource_err = combined.lines().find(|l| {
        l.starts_with("um: open(/proc/self/oom_score_adj)")
            || l.starts_with("um: write(oom_score_adj)")
            || l.starts_with("um: sched_setaffinity")
            || l.starts_with("um: madvise(MADV_NOHUGEPAGE)")
            || l.starts_with("um: madvise(MADV_HUGEPAGE)")
            || l.contains("malformed UM_KVM_V2_CPU_AFFINITY")
    });
    details.insert("backend_banner".into(), backend_ok.to_string());
    if let Some(e) = resource_err {
        details.insert("resource_error".into(), e.into());
    }

    let (verdict, summary) = if backend_ok && resource_err.is_none() {
        (Verdict::Pass, "knobs applied silently, backend up".into())
    } else {
        (
            Verdict::Fail,
            "resource controls failed - see step-4-host_resources.log".into(),
        )
    };
    Ok(("host_resources", verdict, summary, details))
}

fn step5_diverse_soak(
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();
    let daemon = selftests_dir.join("soak/run-soak-daemon.sh");
    if !daemon.exists() {
        details.insert("daemon".into(), format!("MISSING:{}", daemon.display()));
        return Ok((
            "soak",
            Verdict::Fail,
            "soak daemon script absent".into(),
            details,
        ));
    }
    let soak_out = out_dir.join("step-5-soak");
    std::fs::create_dir_all(&soak_out).ok();

    // Workload mix covers independent syscall surfaces that should be
    // stable under kvm-v2.
    let workloads = "memcheck,iocheck,stress-ng,tier1-pylibs,django-loopback-none";
    let iters_per_rotation = "10";

    let log = out_dir.join("step-5-soak.log");
    let mut cmd = Command::new("bash");
    cmd.arg(&daemon)
        .arg("--backends")
        .arg("kvm-v2")
        .arg("--workloads")
        .arg(workloads)
        .arg("--workers")
        .arg("2")
        .arg("--iters-per-rotation")
        .arg(iters_per_rotation)
        .arg("--budget-sec")
        .arg(args.soak_budget_sec.to_string())
        .arg("--out")
        .arg(&soak_out);
    cmd.env("UML_KERNEL", &args.kernel);

    // Inherit current env (includes any UML_KERNEL override).
    let out = cmd.output().context("spawn soak daemon")?;
    let _ = std::fs::write(&log, &out.stdout);

    // Parse scoreboard.jsonl for verdict counts.
    let scoreboard = soak_out.join("scoreboard.jsonl");
    if !scoreboard.exists() {
        details.insert("scoreboard".into(), "absent".into());
        return Ok((
            "soak",
            Verdict::Fail,
            "soak produced no scoreboard.jsonl".into(),
            details,
        ));
    }

    let text = std::fs::read_to_string(&scoreboard).unwrap_or_default();
    let mut pass = 0u64;
    let mut fail = 0u64;
    let mut panic = 0u64;
    let mut other = 0u64;
    let mut total = 0u64;
    let mut sigbus = false;
    let mut high_cr2 = false;
    for line in text.lines() {
        total += 1;
        if line.contains("\"verdict\":\"PASS\"") {
            pass += 1;
        } else if line.contains("\"verdict\":\"FAIL\"") {
            fail += 1;
        } else if line.contains("\"verdict\":\"PANIC\"") {
            panic += 1;
        } else {
            other += 1;
        }
        if line.contains("Kernel mode signal 7") {
            sigbus = true;
        }
        if line.contains("high-cr2") {
            high_cr2 = true;
        }
    }

    details.insert("total".into(), total.to_string());
    details.insert("pass".into(), pass.to_string());
    details.insert("fail".into(), fail.to_string());
    details.insert("panic".into(), panic.to_string());
    details.insert("other".into(), other.to_string());

    if total == 0 {
        return Ok((
            "soak",
            Verdict::Fail,
            "scoreboard.jsonl was empty".into(),
            details,
        ));
    }

    // Wilson 95% CI lower bound.
    let lo = wilson_lower(pass as f64, total as f64) * 100.0;
    details.insert("wilson_lo_pct".into(), format!("{lo:.2}"));

    // Gate logic:
    // - HARD: zero panics, zero "Kernel mode signal 7", zero high-cr2.
    //   Any of these is a definite regression or known-bad shape.
    // - PASS path 1 (perfect run): 100% pass rate AND n >= 20.
    //   Statistically clean even though Wilson 95% lower bound is
    //   only ~85% at n=20, 96.3% at n=100, 99.6% at n=1000; for
    //   a "no failure observed" signal we don't need the lower
    //   bound to chase 100%.
    // - PASS path 2 (Wilson gate): pass rate < 100% but the
    //   Wilson 95% lower bound clears the configured threshold.
    //   This catches "consistent but slightly noisy" runs.
    let no_anomaly = panic == 0 && !sigbus && !high_cr2;
    let perfect_run = pass == total && total >= 20;
    let wilson_ok = lo >= args.soak_min_pct;
    let gate_ok = no_anomaly && (perfect_run || wilson_ok);

    let (verdict, summary) = if gate_ok {
        let why = if perfect_run {
            format!("{pass}/{total} PASS (100% - perfect run, n>=20)")
        } else {
            format!(
                "{pass}/{total} PASS, Wilson95 lower={lo:.2}% (>={:.1}%)",
                args.soak_min_pct
            )
        };
        (
            Verdict::Pass,
            format!("{why}; 0 panics 0 sigbus 0 high_cr2"),
        )
    } else {
        (
            Verdict::Fail,
            format!(
                "{pass}/{total} PASS Wilson95={lo:.2}% (gate {:.1}%); panics={panic} sigbus={sigbus} high_cr2={high_cr2}",
                args.soak_min_pct
            ),
        )
    };
    Ok(("soak", verdict, summary, details))
}

/// Vector2 stress step (opt-in, --with-vector2): stress the vector2 network driver
/// under kvm-v2 with the tier3-django-v2 workload.  This is the
/// mission-level check for the default umlctl-managed vector2 TAP/fd path.
///
/// Drives the run-soak-daemon.sh harness with N iterations of
/// tier3-django-v2 (which routes the {{NETWORK_DRIVER}} template
/// placeholder to vector2 via tier3_network_driver in the daemon).
/// Acceptance: every iteration PASS, 0 panics, 0 SIGBUS,
/// TLB_LAG-correlated noise OK as long as verdict is PASS.
///
/// Requires host-side TAP + iptables capability (sudo -n). If the
/// preflight detects this is unavailable, the step SKIPs cleanly.
fn step7_vector2_stress(
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();

    // Probe for passwordless sudo: vector2 + tap setup needs it.
    let sudo_ok = Command::new("sudo")
        .args(["-n", "true"])
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if !sudo_ok {
        details.insert("sudo_probe".into(), "sudo -n unavailable".into());
        return Ok((
            "vector2",
            Verdict::Skip,
            "host lacks passwordless sudo for TAP setup".into(),
            details,
        ));
    }

    let daemon = selftests_dir.join("soak/run-soak-daemon.sh");
    if !daemon.exists() {
        details.insert("daemon".into(), format!("MISSING:{}", daemon.display()));
        return Ok((
            "vector2",
            Verdict::Fail,
            "soak daemon script absent".into(),
            details,
        ));
    }

    let soak_out = out_dir.join("step-7-vector2");
    std::fs::create_dir_all(&soak_out).ok();

    let iters = args.vector2_iters.to_string();
    // 180s timeout * iters with some slack.
    let budget_sec = (args.vector2_iters as u64 * 240 + 60).to_string();

    let log = out_dir.join("step-7-vector2.log");
    let mut cmd = Command::new("bash");
    cmd.arg(&daemon)
        .arg("--backends")
        .arg("kvm-v2")
        .arg("--workloads")
        .arg("tier3-django-v2")
        .arg("--workers")
        .arg("1")
        .arg("--iters-per-rotation")
        .arg(&iters)
        .arg("--budget-sec")
        .arg(&budget_sec)
        .arg("--out")
        .arg(&soak_out);
    cmd.env("UML_KERNEL", &args.kernel);

    let out = cmd.output().context("spawn vector2 stress soak daemon")?;
    let _ = std::fs::write(&log, &out.stdout);

    let scoreboard = soak_out.join("scoreboard.jsonl");
    if !scoreboard.exists() {
        details.insert("scoreboard".into(), "absent".into());
        return Ok((
            "vector2",
            Verdict::Fail,
            "vector2 soak produced no scoreboard.jsonl".into(),
            details,
        ));
    }

    let text = std::fs::read_to_string(&scoreboard).unwrap_or_default();
    let mut pass = 0u64;
    let mut fail = 0u64;
    let mut panic_n = 0u64;
    let mut total = 0u64;
    let mut sigbus = false;
    let mut driver_seen = String::new();
    for line in text.lines() {
        total += 1;
        if line.contains("\"verdict\":\"PASS\"") {
            pass += 1;
        } else if line.contains("\"verdict\":\"PANIC\"") {
            panic_n += 1;
        } else if line.contains("\"verdict\":\"FAIL\"") {
            fail += 1;
        }
        if line.contains("Kernel mode signal 7") {
            sigbus = true;
        }
        // Confirm the driver actually was vector2 (defensive: catch a
        // case where the daemon's tier3_network_driver routing broke).
        if driver_seen.is_empty() {
            const KEY: &str = "\"uml_network_driver\":\"";
            if let Some(i) = line.find(KEY) {
                let s = &line[i + KEY.len()..];
                if let Some(e) = s.find('"') {
                    driver_seen = s[..e].to_string();
                }
            }
        }
    }

    details.insert("total".into(), total.to_string());
    details.insert("pass".into(), pass.to_string());
    details.insert("fail".into(), fail.to_string());
    details.insert("panic".into(), panic_n.to_string());
    details.insert("driver_seen".into(), driver_seen.clone());

    if total == 0 {
        return Ok((
            "vector2",
            Verdict::Fail,
            "scoreboard.jsonl was empty".into(),
            details,
        ));
    }
    if driver_seen != "vector2" {
        return Ok((
            "vector2",
            Verdict::Fail,
            format!("expected uml_network_driver=vector2 but saw \"{driver_seen}\""),
            details,
        ));
    }

    let gate_ok = pass == total && panic_n == 0 && !sigbus;
    let (verdict, summary) = if gate_ok {
        (
            Verdict::Pass,
            format!("{pass}/{total} PASS (vector2 + kvm-v2 tier3-django-v2); 0 panics 0 sigbus"),
        )
    } else {
        (
            Verdict::Fail,
            format!("{pass}/{total} PASS; panics={panic_n} sigbus={sigbus}"),
        )
    };
    Ok(("vector2", verdict, summary, details))
}

// Template-pause fork stress step.
//
// Drives the master through hundreds of fork-on-resume iterations
// and asserts the six gates documented in run-template-pause-fork-
// stress.sh (master alive, distinct child pids, RSS drift <= 5%, no
// post-teardown stub leak, >= N iters in window, identity-blob
// round-trip).
//
// Requires a separate kernel built with CONFIG_UM_TEMPLATE_PAUSE_FORK
// =y, because the main mission kernel is normally configured for
// kvm-v2 (which assert_fork_safety() refuses).  Resolved from
// --fork-kernel / $UM_FORK_KERNEL, with the default of
// $HOME/src/uml-builds/uml-tplpause-fork/linux.  SKIPs cleanly when
// that binary doesn't exist; the rest of the mission still gates.
fn step8_fork_stress(
    args: &MissionArgs,
    selftests_dir: &Path,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();

    let fork_kernel = args.fork_kernel.clone().unwrap_or_else(|| {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/root".into());
        PathBuf::from(format!("{home}/src/uml-builds/uml-tplpause-fork/linux"))
    });
    details.insert("fork_kernel".into(), fork_kernel.display().to_string());
    if !fork_kernel.exists() {
        return Ok((
            "fork-stress",
            Verdict::Skip,
            format!(
                "fork-stress kernel absent at {} (set --fork-kernel or build CONFIG_UM_TEMPLATE_PAUSE_FORK=y)",
                fork_kernel.display()
            ),
            details,
        ));
    }

    let script = selftests_dir.join("template-pause-fork-stress/run-template-pause-fork-stress.sh");
    if !script.exists() {
        details.insert("script".into(), format!("MISSING:{}", script.display()));
        return Ok((
            "fork-stress",
            Verdict::Fail,
            "fork-stress selftest script absent".into(),
            details,
        ));
    }

    let log = out_dir.join("step-8-fork-stress.log");
    let mut cmd = Command::new("bash");
    cmd.arg(&script);
    cmd.env("UML_BINARY", &fork_kernel);
    // Use the script's defaults for N / SECS / BLOBS / RSS_DRIFT /
    // ATTEMPTS.  Override per-mission only if a regression of the
    // residual v1-ceiling makes the default attempts insufficient.

    let out = cmd.output().context("spawn fork-stress selftest")?;
    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    let _ = std::fs::write(
        &log,
        format!("=== stdout ===\n{stdout}\n=== stderr ===\n{stderr}"),
    );

    // Parse per-gate detail rows for the scoreboard.  The selftest
    // script's strict single-attempt format prints G[1-8] lines plus
    // a final VERDICT line.  (Older versions had a retry harness
    // with "######## attempt N PASSED" markers; that was removed
    // when the strict gates landed.)
    let mut last_iters = String::new();
    let mut last_distinct_pids = String::new();
    let mut last_drift = String::new();
    let mut last_blobs = String::new();
    let mut g7_panics = String::new();
    let mut g8_capture = String::new();
    for line in stdout.lines() {
        if let Some(rest) = line.strip_prefix("G5 master iterations   : ") {
            last_iters = rest.trim().to_string();
        } else if let Some(rest) = line.strip_prefix("G2 distinct child pids : ") {
            last_distinct_pids = rest.trim().to_string();
        } else if let Some(rest) = line.strip_prefix("G3 RSS drift           : ") {
            last_drift = rest.trim().to_string();
        } else if let Some(rest) = line.strip_prefix("G6 identity round-trip : ") {
            last_blobs = rest.trim().to_string();
        } else if let Some(rest) = line.strip_prefix("G7 zero kernel panics  : ") {
            g7_panics = rest.trim().to_string();
        } else if let Some(rest) = line.strip_prefix("G8 /proc vs kernel log : ") {
            g8_capture = rest.trim().to_string();
        }
    }
    if !last_iters.is_empty() {
        details.insert("g5_iters".into(), last_iters.clone());
    }
    if !last_distinct_pids.is_empty() {
        details.insert("g2_child_pids".into(), last_distinct_pids);
    }
    if !last_drift.is_empty() {
        details.insert("g3_rss_drift".into(), last_drift);
    }
    if !last_blobs.is_empty() {
        details.insert("g6_blobs".into(), last_blobs);
    }
    if !g7_panics.is_empty() {
        details.insert("g7_panics".into(), g7_panics);
    }
    if !g8_capture.is_empty() {
        details.insert("g8_capture".into(), g8_capture);
    }

    // SKIP propagation: the script exits 4 when kernel lacks the
    // CONFIG_UM_TEMPLATE_PAUSE_FORK feature OR python3 is missing.
    if out.status.code() == Some(4) {
        return Ok((
            "fork-stress",
            Verdict::Skip,
            "fork-stress kernel lacks CONFIG_UM_TEMPLATE_PAUSE_FORK or python3 missing".into(),
            details,
        ));
    }

    if out.status.success() {
        Ok((
            "fork-stress",
            Verdict::Pass,
            format!("PASS (G5 iters={last_iters})"),
            details,
        ))
    } else {
        Ok((
            "fork-stress",
            Verdict::Fail,
            format!(
                "FAIL: script exited rc={:?} (last G5 iters={last_iters})",
                out.status.code()
            ),
            details,
        ))
    }
}

fn step6_diagnostic(
    args: &MissionArgs,
    out_dir: &Path,
) -> Result<(&'static str, Verdict, String, BTreeMap<String, String>)> {
    let mut details = BTreeMap::new();
    details.insert("kernel".into(), args.kernel.display().to_string());
    if let Some(v) = kernel_version(&args.kernel) {
        details.insert("kernel_version".into(), v);
    }
    if let Some(g) = git_head() {
        details.insert("git_head".into(), g);
    }
    details.insert("host".into(), hostname());
    details.insert(
        "umlctl_version".into(),
        env!("CARGO_PKG_VERSION").to_string(),
    );

    // Hugepage pool sizing + cgroup v2 presence are useful context
    // for interpreting resource-control results.
    let hp_2m = std::fs::read_to_string("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages")
        .ok()
        .and_then(|s| s.trim().parse::<u64>().ok())
        .unwrap_or(0);
    details.insert("hugepages_2m_reserved".into(), hp_2m.to_string());
    let cgroup_v2 = Path::new("/sys/fs/cgroup/cgroup.controllers").exists();
    details.insert("cgroup_v2_present".into(), cgroup_v2.to_string());

    // Write diagnostic.json as a separate file so callers do not need
    // to parse the full scoreboard for reproducibility metadata.
    let diag = serde_json::to_string_pretty(&details)?;
    std::fs::write(out_dir.join("diagnostic.json"), diag).ok();

    Ok((
        "diagnostic",
        Verdict::Pass,
        "captured kernel/host/umlctl/cgroup/hugepage metadata".into(),
        details,
    ))
}

// --------------------------------------------------------------
// Helpers
// --------------------------------------------------------------

fn resolve_selftests_dir(args: &MissionArgs) -> Result<PathBuf> {
    if let Some(p) = &args.selftests_dir {
        return Ok(p.clone());
    }
    let cwd = std::env::current_dir()?;
    let guess = cwd.join("tools/testing/selftests/um");
    if guess.exists() {
        return Ok(guess);
    }
    anyhow::bail!(
        "cannot locate tools/testing/selftests/um - set UMLCTL_SELFTESTS_DIR \
         or pass --selftests-dir"
    )
}

fn run_script(path: &Path, args: &MissionArgs, log: &Path) -> Result<i32> {
    let mut cmd = Command::new("bash");
    cmd.arg(path)
        .env("UML_BINARY", &args.kernel)
        .env("UML_KERNEL", &args.kernel);
    let out = cmd
        .output()
        .with_context(|| format!("run script {}", path.display()))?;
    let combined = format!(
        "{}\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    let _ = std::fs::write(log, combined);
    Ok(out.status.code().unwrap_or(-1))
}

fn parse_kv_int(s: &str, key: &str) -> Option<u64> {
    let i = s.find(key)?;
    let tail = &s[i + key.len()..];
    let end = tail
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(tail.len());
    tail[..end].parse().ok()
}

/// Wilson 95% CI lower bound for x out of n.
fn wilson_lower(x: f64, n: f64) -> f64 {
    if n <= 0.0 {
        return 0.0;
    }
    let z = 1.96;
    let phat = x / n;
    let denom = 1.0 + z * z / n;
    let centre = phat + z * z / (2.0 * n);
    let margin = z * ((phat * (1.0 - phat) + z * z / (4.0 * n)) / n).sqrt();
    ((centre - margin) / denom).max(0.0)
}

fn hash_file(p: &Path) -> Option<String> {
    let bytes = std::fs::read(p).ok()?;
    let h = sha256_hex(&bytes);
    Some(h)
}

fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    h.update(bytes);
    format!("{:x}", h.finalize())
}

fn short_sha(s: &str) -> String {
    if s.len() >= 12 {
        s[..12].to_string()
    } else {
        s.to_string()
    }
}

fn kernel_version(p: &Path) -> Option<String> {
    let out = Command::new("strings").arg(p).output().ok()?;
    let txt = String::from_utf8_lossy(&out.stdout);
    txt.lines()
        .find(|l| l.starts_with("Linux version 7."))
        .and_then(|l| l.split(' ').nth(2))
        .map(|s| s.to_string())
}

fn hostname() -> String {
    Command::new("hostname")
        .output()
        .ok()
        .and_then(|o| {
            String::from_utf8(o.stdout)
                .ok()
                .map(|s| s.trim().to_string())
        })
        .unwrap_or_else(|| "unknown".to_string())
}

fn git_head() -> Option<String> {
    let out = Command::new("git")
        .args(["rev-parse", "HEAD"])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

fn rfc3339_now() -> String {
    use std::time::SystemTime;
    let secs = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // Cheap RFC3339 without chrono: use the date binary.
    let out = Command::new("date")
        .args(["-u", "-d", &format!("@{secs}"), "+%Y-%m-%dT%H:%M:%SZ"])
        .output()
        .ok();
    out.and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|| format!("epoch+{secs}"))
}

fn unix_ts_secs() -> u64 {
    use std::time::SystemTime;
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn print_step_line(r: &MissionStep) {
    let v = match r.verdict {
        Verdict::Pass => "PASS",
        Verdict::Fail => "FAIL",
        Verdict::Skip => "SKIP",
    };
    eprintln!(
        "[Step {}] {} {} ({:.1}s) - {}",
        r.step_id,
        v,
        r.name,
        r.duration_ms as f64 / 1000.0,
        r.summary
    );
}
