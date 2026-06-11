// SPDX-License-Identifier: GPL-2.0
//
// umlctl gate - sealed wrappers around existing UML test harnesses.
//
// The contract: a Gatefile describes how to invoke a pre-existing
// harness (run-regrtest-repros.sh, run-perf-getpid.sh, kunit, cargo
// test, ...) and how to extract PASS/FAIL/EXPECTED_FAIL counts from its
// stdout. The gate runner does NOT define passing or failing - it
// reports what the harness said. Tightening thresholds is progress,
// loosening must be a visible TOML diff.
//
// Output: one row appended to tools/testing/selftests/um/
// scoreboard.jsonl per run. `umlctl gate diff` shows trend so a
// silent regression (PASS count dropping commit-over-commit) can't
// hide.

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

// -------------------------------------------------------------------
// Gatefile schema
// -------------------------------------------------------------------

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Gatefile {
    pub schema_version: u32,
    pub gate: GateMeta,
    pub run: RunSpec,
    #[serde(default)]
    pub parse: ParseSpec,
    #[serde(default)]
    pub thresholds: Thresholds,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct GateMeta {
    pub name: String,
    #[serde(default)]
    pub description: String,
    /// Optional group tag such as "performance". Informational;
    /// lets `gate diff` group related gates.
    #[serde(default)]
    pub phase: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RunSpec {
    /// Command to execute. Run via /bin/bash -c so the full string
    /// is the script - that lets descriptors like `make -C ...` work
    /// unchanged.
    pub cmd: String,
    /// Working directory; defaults to the linux source root if empty.
    #[serde(default)]
    pub cwd: String,
    /// Per-run env. Values support `{{backend}}` and `{{kernel}}`
    /// substitution from the CLI args.
    #[serde(default)]
    pub env: BTreeMap<String, String>,
    /// Wall-clock budget. The runner sends SIGTERM at this point and
    /// records exit_code = 124 (matching coreutils `timeout`).
    #[serde(default = "default_budget")]
    pub budget_sec: u64,
}
fn default_budget() -> u64 {
    600
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
pub struct ParseSpec {
    /// If set, every line that matches counts as one PASS.
    #[serde(default)]
    pub pass_regex: String,
    /// If set, every line that matches counts as one FAIL.
    #[serde(default)]
    pub fail_regex: String,
    /// If set, every line that matches counts as one EXPECTED_FAIL.
    #[serde(default)]
    pub expected_fail_regex: String,
    /// If true (default false), exit code 0 means PASS=1/FAIL=0 and
    /// non-zero means PASS=0/FAIL=1. Useful for harnesses that don't
    /// emit per-test lines (cargo test, kunit_tool).
    #[serde(default)]
    pub pass_on_exit_zero: bool,
    /// Metric extractors. Each captures one numeric value from stdout
    /// for trend tracking. Stored verbatim in the scoreboard row.
    #[serde(default)]
    pub metrics: Vec<MetricSpec>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MetricSpec {
    pub name: String,
    pub regex: String,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
pub struct Thresholds {
    /// Minimum required PASS count. The gate FAILs if the harness
    /// reports fewer. To loosen this number, edit the TOML - that
    /// shows up in git history as an explicit policy change.
    #[serde(default)]
    pub min_pass: Option<u32>,
    /// Maximum tolerated FAIL count.
    #[serde(default)]
    pub max_fail: Option<u32>,
    /// If set, the gate FAILs when the harness exits non-zero, even
    /// if PASS/FAIL counts would otherwise meet thresholds. Default
    /// true - exit code is the most authoritative pass/fail signal a
    /// harness can give us.
    #[serde(default = "default_require_clean_exit")]
    pub require_clean_exit: bool,
}
fn default_require_clean_exit() -> bool {
    true
}

impl Gatefile {
    pub fn from_path(path: &Path) -> Result<Self> {
        let s = std::fs::read_to_string(path)
            .with_context(|| format!("read gatefile {}", path.display()))?;
        let g: Gatefile =
            toml::from_str(&s).with_context(|| format!("parse gatefile {}", path.display()))?;
        if g.schema_version != 1 {
            bail!(
                "unsupported gatefile schema_version {} (only 1)",
                g.schema_version
            );
        }
        Ok(g)
    }
}

// -------------------------------------------------------------------
// Run + collect
// -------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Row {
    /// ISO-8601 UTC timestamp.
    pub ts: String,
    pub gate: String,
    pub backend: String,
    pub commit: String,
    pub branch: String,
    pub kernel: String,
    pub host: String,
    pub run_id: String,
    pub pass: u32,
    pub fail: u32,
    pub expected_fail: u32,
    pub exit_code: i32,
    pub duration_sec: f64,
    pub thresholds_met: bool,
    /// Free-form metric snapshots from ParseSpec.metrics.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub metrics: BTreeMap<String, String>,
    /// Empty on PASS; one short reason per failure mode otherwise.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub failures: Vec<String>,
}

pub struct RunInputs<'a> {
    pub gate: &'a Gatefile,
    pub backend: &'a str,
    pub kernel: &'a str,
    pub source_root: &'a Path,
}

pub fn run(inp: RunInputs) -> Result<Row> {
    let cmd_str = expand(&inp.gate.run.cmd, inp.backend, inp.kernel);
    let cwd = if inp.gate.run.cwd.is_empty() {
        inp.source_root.to_path_buf()
    } else {
        let p = PathBuf::from(expand(&inp.gate.run.cwd, inp.backend, inp.kernel));
        if p.is_absolute() {
            p
        } else {
            inp.source_root.join(p)
        }
    };

    // Wall-clock cap via coreutils timeout(1). SIGTERM at the
    // budget, SIGKILL 5s later. We pass through a layer of
    // /bin/bash -lc so the gate cmd can use shell features
    // (pipes, redirects) without quoting gymnastics.
    let wrapped = format!(
        "timeout --kill-after=5 {sec} bash -lc {script}",
        sec = inp.gate.run.budget_sec,
        script = shell_quote(&cmd_str),
    );
    let mut cmd = Command::new("/bin/bash");
    cmd.arg("-lc").arg(&wrapped).current_dir(&cwd);
    for k in ["PATH", "HOME", "USER", "LANG", "TERM"] {
        if let Ok(v) = std::env::var(k) {
            cmd.env(k, v);
        }
    }
    for (k, v) in &inp.gate.run.env {
        cmd.env(k, expand(v, inp.backend, inp.kernel));
    }

    let started = Instant::now();
    let out = cmd
        .output()
        .with_context(|| format!("spawn {:?}", cmd_str))?;
    let duration_sec = started.elapsed().as_secs_f64();
    let exit_code = out.status.code().unwrap_or(-1);
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    let combined = format!("{}{}", stdout, stderr);

    let (pass, fail, expected_fail) = count_results(&inp.gate.parse, &combined, exit_code);
    let metrics = extract_metrics(&inp.gate.parse, &combined)?;

    let mut failures: Vec<String> = Vec::new();
    if let Some(min) = inp.gate.thresholds.min_pass {
        if pass < min {
            failures.push(format!("min_pass: {pass} < {min}"));
        }
    }
    if let Some(max) = inp.gate.thresholds.max_fail {
        if fail > max {
            failures.push(format!("max_fail: {fail} > {max}"));
        }
    }
    if inp.gate.thresholds.require_clean_exit && exit_code != 0 {
        failures.push(format!("exit_code: {exit_code}"));
    }
    let thresholds_met = failures.is_empty();

    let row = Row {
        ts: now_iso8601(),
        gate: inp.gate.gate.name.clone(),
        backend: inp.backend.to_string(),
        commit: git_short_rev(inp.source_root).unwrap_or_else(|| "unknown".into()),
        branch: git_branch(inp.source_root).unwrap_or_else(|| "detached".into()),
        kernel: inp.kernel.to_string(),
        host: hostname().unwrap_or_else(|| "unknown".into()),
        run_id: crate::run::generate_run_id(),
        pass,
        fail,
        expected_fail,
        exit_code,
        duration_sec,
        thresholds_met,
        metrics,
        failures,
    };
    Ok(row)
}

fn count_results(p: &ParseSpec, output: &str, exit_code: i32) -> (u32, u32, u32) {
    if p.pass_on_exit_zero && p.pass_regex.is_empty() && p.fail_regex.is_empty() {
        return if exit_code == 0 { (1, 0, 0) } else { (0, 1, 0) };
    }
    let pass = if p.pass_regex.is_empty() {
        0
    } else {
        count_lines_matching(&p.pass_regex, output)
    };
    let fail = if p.fail_regex.is_empty() {
        0
    } else {
        count_lines_matching(&p.fail_regex, output)
    };
    let xfail = if p.expected_fail_regex.is_empty() {
        0
    } else {
        count_lines_matching(&p.expected_fail_regex, output)
    };
    (pass, fail, xfail)
}

fn count_lines_matching(pattern: &str, output: &str) -> u32 {
    // Shell out to grep so the regex syntax matches what harness
    // authors expect (POSIX ERE). Pulling in the regex crate would
    // balloon the dep tree for ~5 patterns.
    use std::io::Write;
    use std::process::Stdio;
    let Ok(mut child) = Command::new("grep")
        .arg("-cE")
        .arg(pattern)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
    else {
        return 0;
    };
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(output.as_bytes());
    }
    let Ok(out) = child.wait_with_output() else {
        return 0;
    };
    String::from_utf8_lossy(&out.stdout)
        .trim()
        .parse::<u32>()
        .unwrap_or(0)
}

fn extract_metrics(p: &ParseSpec, output: &str) -> Result<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    for m in &p.metrics {
        if let Some(v) = first_capture(&m.regex, output) {
            out.insert(m.name.clone(), v);
        }
    }
    Ok(out)
}

fn first_capture(pattern: &str, text: &str) -> Option<String> {
    // sed -nE 's/.../\1/p' returns first capture across lines; we
    // take the first match. Same rationale as count_lines_matching:
    // avoid a regex-crate dep for a handful of patterns.
    use std::io::Write;
    use std::process::Stdio;
    let script = format!("s/.*{}.*/\\1/p", pattern);
    let mut child = Command::new("sed")
        .arg("-nE")
        .arg(&script)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    if let Some(stdin) = child.stdin.as_mut() {
        let _ = stdin.write_all(text.as_bytes());
    }
    let out = child.wait_with_output().ok()?;
    let s = String::from_utf8(out.stdout).ok()?;
    s.lines().next().map(|l| l.trim().to_string())
}

// -------------------------------------------------------------------
// scoreboard.jsonl
// -------------------------------------------------------------------

pub fn append_scoreboard(path: &Path, row: &Row) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).ok();
    }
    let mut line = serde_json::to_string(row)?;
    line.push('\n');
    use std::io::Write;
    let mut f = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .with_context(|| format!("open {}", path.display()))?;
    f.write_all(line.as_bytes())?;
    Ok(())
}

pub fn read_scoreboard(path: &Path) -> Result<Vec<Row>> {
    let s = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).context(format!("read {}", path.display())),
    };
    let mut rows = Vec::new();
    for (i, ln) in s.lines().enumerate() {
        if ln.trim().is_empty() {
            continue;
        }
        let row: Row = serde_json::from_str(ln)
            .with_context(|| format!("parse {}:{}", path.display(), i + 1))?;
        rows.push(row);
    }
    Ok(rows)
}

// -------------------------------------------------------------------
// Render: human-readable diff
// -------------------------------------------------------------------

pub fn render_diff(rows: &[Row], gate_filter: Option<&str>, n: usize) -> String {
    let filtered: Vec<&Row> = rows
        .iter()
        .filter(|r| gate_filter.is_none_or(|g| r.gate == g))
        .collect();
    let take = filtered.len().saturating_sub(n);
    let recent = &filtered[take..];
    if recent.is_empty() {
        return "no scoreboard rows match\n".into();
    }
    let mut out = String::new();
    out.push_str(
        "ts                 gate                            backend  pass fail xfail  exit  dur(s)  status\n",
    );
    let mut prev_pass: BTreeMap<(String, String), u32> = BTreeMap::new();
    for r in recent {
        let key = (r.gate.clone(), r.backend.clone());
        let delta = match prev_pass.get(&key) {
            None => "    ".to_string(),
            Some(&p) if r.pass > p => format!(" +{:>2}", r.pass - p),
            Some(&p) if r.pass < p => format!(" -{:>2}", p - r.pass),
            Some(_) => "  ==".to_string(),
        };
        prev_pass.insert(key, r.pass);
        let status = if r.thresholds_met { "PASS" } else { "FAIL" };
        out.push_str(&format!(
            "{ts:19} {gate:30} {backend:8} {pass:>4}{delta} {fail:>4} {xfail:>5}  {exit:>4}  {dur:>6.1}  {status}\n",
            ts = &r.ts[..19.min(r.ts.len())],
            gate = trunc(&r.gate, 30),
            backend = trunc(&r.backend, 8),
            pass = r.pass,
            delta = delta,
            fail = r.fail,
            xfail = r.expected_fail,
            exit = r.exit_code,
            dur = r.duration_sec,
            status = status,
        ));
        if !r.failures.is_empty() {
            for f in &r.failures {
                out.push_str(&format!("    +- {}\n", f));
            }
        }
    }
    out
}

fn trunc(s: &str, n: usize) -> &str {
    if s.len() <= n {
        s
    } else {
        &s[..n]
    }
}

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

fn expand(s: &str, backend: &str, kernel: &str) -> String {
    s.replace("{{backend}}", backend)
        .replace("{{kernel}}", kernel)
}

fn shell_quote(s: &str) -> String {
    // Single-quote and escape embedded single quotes.
    let mut out = String::with_capacity(s.len() + 2);
    out.push('\'');
    for ch in s.chars() {
        if ch == '\'' {
            out.push_str("'\\''");
        } else {
            out.push(ch);
        }
    }
    out.push('\'');
    out
}

fn now_iso8601() -> String {
    use time::format_description::well_known::Rfc3339;
    time::OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_default()
}

fn git_short_rev(root: &Path) -> Option<String> {
    let out = Command::new("git")
        .args(["rev-parse", "--short=12", "HEAD"])
        .current_dir(root)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

fn git_branch(root: &Path) -> Option<String> {
    let out = Command::new("git")
        .args(["rev-parse", "--abbrev-ref", "HEAD"])
        .current_dir(root)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if s == "HEAD" {
        // Detached - fall back to the short rev so the row is still
        // attributable.
        git_short_rev(root)
    } else {
        Some(s)
    }
}

fn hostname() -> Option<String> {
    let out = Command::new("hostname").output().ok()?;
    if !out.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_minimal_gatefile() {
        let toml = r#"
schema_version = 1
[gate]
name = "demo"
[run]
cmd = "true"
[parse]
pass_on_exit_zero = true
[thresholds]
"#;
        let g: Gatefile = toml::from_str(toml).unwrap();
        assert_eq!(g.gate.name, "demo");
        assert!(g.parse.pass_on_exit_zero);
    }

    #[test]
    fn unknown_field_rejected() {
        let toml = r#"
schema_version = 1
[gate]
name = "x"
yolo = true
[run]
cmd = "true"
"#;
        assert!(toml::from_str::<Gatefile>(toml).is_err());
    }

    #[test]
    fn count_results_exit_zero_path() {
        let p = ParseSpec {
            pass_on_exit_zero: true,
            ..Default::default()
        };
        assert_eq!(count_results(&p, "", 0), (1, 0, 0));
        assert_eq!(count_results(&p, "", 1), (0, 1, 0));
    }

    #[test]
    fn count_results_regex_path() {
        let p = ParseSpec {
            pass_regex: r"^REPRO: \S+ PASS".into(),
            fail_regex: r"^REPRO: \S+ FAIL".into(),
            expected_fail_regex: r"^REPRO: \S+ EXPECTED_FAIL".into(),
            ..Default::default()
        };
        let txt = "\
REPRO: hello PASS
REPRO: fork_exec_wait FAIL
REPRO: ensurepip_check EXPECTED_FAIL
REPRO: sigaction PASS
some-noise-line
";
        assert_eq!(count_results(&p, txt, 0), (2, 1, 1));
    }

    #[test]
    fn metrics_extract_first_capture() {
        let p = ParseSpec {
            pass_on_exit_zero: true,
            metrics: vec![MetricSpec {
                name: "ratio".into(),
                regex: r"ratio_kvm_over_seccomp=([0-9.]+)".into(),
            }],
            ..Default::default()
        };
        let txt = "PERF_GETPID: SUMMARY backends=[seccomp,kvm] kvm_cyc=320 seccomp_cyc=553 ratio_kvm_over_seccomp=0.578 max_allowed=2.5";
        let m = extract_metrics(&p, txt).unwrap();
        assert_eq!(m.get("ratio").map(|s| s.as_str()), Some("0.578"));
    }

    #[test]
    fn scoreboard_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("scoreboard.jsonl");
        let row = Row {
            ts: "1970-01-01T00:00:00Z".into(),
            gate: "regrtest-class-a-env".into(),
            backend: "seccomp".into(),
            commit: "deadbeef0000".into(),
            branch: "umlctl-deploy".into(),
            kernel: "/tmp/k".into(),
            host: "x".into(),
            run_id: "01ABC".into(),
            pass: 25,
            fail: 3,
            expected_fail: 3,
            exit_code: 0,
            duration_sec: 47.0,
            thresholds_met: true,
            metrics: BTreeMap::new(),
            failures: vec![],
        };
        append_scoreboard(&path, &row).unwrap();
        append_scoreboard(
            &path,
            &Row {
                pass: 26,
                ..row.clone()
            },
        )
        .unwrap();
        let rows = read_scoreboard(&path).unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[1].pass, 26);
    }

    #[test]
    fn diff_marks_pass_delta() {
        let mk = |pass| Row {
            ts: "1970-01-01T00:00:00Z".into(),
            gate: "g".into(),
            backend: "seccomp".into(),
            commit: "c".into(),
            branch: "b".into(),
            kernel: "k".into(),
            host: "h".into(),
            run_id: "r".into(),
            pass,
            fail: 0,
            expected_fail: 0,
            exit_code: 0,
            duration_sec: 1.0,
            thresholds_met: true,
            metrics: BTreeMap::new(),
            failures: vec![],
        };
        let rows = vec![mk(25), mk(26), mk(24)];
        let s = render_diff(&rows, None, 10);
        // Should show one +1 and one -2 marker.
        assert!(s.contains("+ 1"), "expected +1 marker in:\n{s}");
        assert!(s.contains("- 2"), "expected -2 marker in:\n{s}");
    }

    #[test]
    fn shell_quote_handles_singletons() {
        assert_eq!(shell_quote("hello"), "'hello'");
        assert_eq!(shell_quote("it's me"), "'it'\\''s me'");
    }
}
