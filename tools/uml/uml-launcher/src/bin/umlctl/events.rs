// SPDX-License-Identifier: GPL-2.0
//
// Structured event emission for the observability spine
// (Documentation/virt/uml/redesign/08-future-phases/13-uml-
// observability-spine.md, Phase O1.3).
//
// Every spine event lands as one JSON object per line in
// `$STATE/runs/<run_id>/events.jsonl`. Fields follow the
// ECS-shaped header:
//
//   @timestamp, host_ts_ns, run_id, instance, schema,
//   event.category, event.severity, event.action
//
// plus schema-specific extensions (the `extra` field of
// `Event`) flattened into the output object.
//
// Minimal registry of declared schemas for O1.3: `uml.lifecycle.v1`
// is emitted from umlctl itself on create/start/stop/rm;
// `uml.panic.v1` and `uml.oom.v1` are declared + documented
// but not yet populated — their sources (dmesg parser, control
// socket) land in later phases. Keeping the schemas declared
// means consumer tooling can be written against a stable
// surface before the producers exist.

use anyhow::{anyhow, Context, Result};
use serde::Serialize;
use std::io::Write;

use super::manifest;
use super::paths::Paths;
use super::run;
use super::{AssertArgs, EventsArgs};

/// A single spine event. `extra` carries the schema-specific
/// payload as a pre-serialized JSON object; it's spliced into
/// the emitted line after the header fields so the JSONL is
/// flat (no nested object per event).
#[derive(Debug)]
pub struct Event<'a> {
    pub schema: &'static str,
    pub category: &'static str,
    pub severity: Severity,
    pub action: &'a str,
    pub instance: &'a str,
    pub extra: Option<serde_json::Value>,
}

#[derive(Debug, Clone, Copy)]
#[allow(dead_code)] // Warning/Error land when sanitizer/panic emitters arrive.
pub enum Severity {
    Info,
    Warning,
    Error,
}

impl Severity {
    fn as_str(self) -> &'static str {
        match self {
            Severity::Info => "info",
            Severity::Warning => "warning",
            Severity::Error => "error",
        }
    }
}

#[derive(Serialize)]
struct Header<'a> {
    #[serde(rename = "@timestamp")]
    timestamp: String,
    host_ts_ns: u64,
    run_id: &'a str,
    instance: &'a str,
    schema: &'static str,
    #[serde(rename = "event.category")]
    category: &'static str,
    #[serde(rename = "event.severity")]
    severity: &'static str,
    #[serde(rename = "event.action")]
    action: &'a str,
}

/// Append `event` to `$STATE/runs/<run_id>/events.jsonl`. Missing
/// bundle directory is created on demand (defensive — the
/// bundle should exist post-start, but the spine should never
/// fail-loud just because a consumer raced a rm).
pub fn emit(paths: &Paths, run_id: &str, event: Event<'_>) -> Result<()> {
    if run_id.is_empty() {
        // No run_id context — nothing to correlate against.
        // Caller must have handed us a live run_id; log this
        // as a no-op and move on rather than surface to the
        // user.
        return Ok(());
    }

    let dir = paths.run_dir(run_id);
    std::fs::create_dir_all(&dir)
        .with_context(|| format!("create run dir {}", dir.display()))?;

    let header = Header {
        timestamp: manifest::now_rfc3339(),
        host_ts_ns: run::boottime_ns(),
        run_id,
        instance: event.instance,
        schema: event.schema,
        category: event.category,
        severity: event.severity.as_str(),
        action: event.action,
    };

    // Serialize header + extras into a single flat JSON object.
    let mut header_value = serde_json::to_value(&header).context("serialize event header")?;
    if let Some(extra) = event.extra {
        if let (Some(base_obj), Some(extra_obj)) =
            (header_value.as_object_mut(), extra.as_object())
        {
            for (k, v) in extra_obj {
                base_obj.insert(k.clone(), v.clone());
            }
        }
    }
    let line = serde_json::to_string(&header_value).context("serialize event line")?;

    let path = dir.join("events.jsonl");
    let mut f = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .with_context(|| format!("open {}", path.display()))?;
    writeln!(f, "{line}").context("write event line")?;
    Ok(())
}

/// `umlctl events` dispatch. Resolves the bundle from a
/// name-or-run-id, applies filters + --since + --tail, prints
/// matching lines; optionally follows.
pub fn cmd_tail(paths: &Paths, args: &EventsArgs) -> Result<()> {
    let run_id = resolve_name_or_run_id(paths, &args.name_or_run_id);
    let events_path = paths.run_dir(&run_id).join("events.jsonl");
    if !events_path.exists() {
        eprintln!("umlctl: events.jsonl missing for run {}", run_id);
        std::process::exit(7);
    }

    let filters = parse_filters(&args.filters)?;
    let since = args
        .since
        .as_deref()
        .map(parse_since)
        .transpose()
        .context("parse --since")?;

    // Read existing content, apply tail/since/filters.
    let content = std::fs::read_to_string(&events_path)
        .with_context(|| format!("read {}", events_path.display()))?;
    let lines: Vec<&str> = content.lines().collect();
    let mut matched: Vec<String> = Vec::new();
    for line in &lines {
        if line.trim().is_empty() {
            continue;
        }
        let parsed: serde_json::Value = match serde_json::from_str(line) {
            Ok(v) => v,
            Err(_) => continue, // Skip corrupt lines.
        };
        if !event_matches(&parsed, &filters) {
            continue;
        }
        if let Some(since) = &since {
            if !event_is_after(&parsed, since) {
                continue;
            }
        }
        matched.push((*line).to_string());
    }

    let start = if args.tail > 0 && args.tail < matched.len() {
        matched.len() - args.tail
    } else {
        0
    };
    for line in &matched[start..] {
        println!("{line}");
    }

    if args.follow {
        follow_events(&events_path, &filters, since.as_ref())?;
    }
    Ok(())
}

#[derive(Debug)]
struct FilterExpr {
    key: String,
    value: String,
}

fn parse_filters(raw: &[String]) -> Result<Vec<FilterExpr>> {
    let mut out = Vec::with_capacity(raw.len());
    for s in raw {
        let (k, v) = s
            .split_once('=')
            .ok_or_else(|| anyhow!("filter must be K=V, got {s:?}"))?;
        out.push(FilterExpr {
            key: k.to_string(),
            value: v.to_string(),
        });
    }
    Ok(out)
}

fn event_matches(ev: &serde_json::Value, filters: &[FilterExpr]) -> bool {
    let Some(obj) = ev.as_object() else {
        return false;
    };
    for f in filters {
        let actual = match obj.get(&f.key) {
            Some(v) => match v {
                serde_json::Value::String(s) => s.clone(),
                other => other.to_string(),
            },
            None => return false,
        };
        // Allow filter values to match either the exact string
        // or the JSON-literal form (for numbers), so both
        // `pid=12345` and `schema=uml.panic.v1` work the same.
        let want = &f.value;
        if actual != *want && actual.trim_matches('"') != want.as_str() {
            return false;
        }
    }
    true
}

/// `--since` accepts `30s|5m|2h|1d` relative or RFC3339 absolute.
/// Returns a pair `(host_ts_ns_floor, wall_ts_floor_rfc3339)`;
/// events pass if *either* comparison succeeds (we accept the
/// later of the two clocks so operators don't get tripped by
/// a clock skew between host_ts_ns and @timestamp).
enum Since {
    /// CLOCK_BOOTTIME floor in nanoseconds.
    HostNs(u64),
    /// RFC3339 wall-clock floor.
    Wall(String),
}

fn parse_since(s: &str) -> Result<Since> {
    if let Some(d) = parse_duration(s) {
        let now_ns = run::boottime_ns();
        let floor = now_ns.saturating_sub(d);
        return Ok(Since::HostNs(floor));
    }
    // Assume RFC3339; defer to time's parser for validation.
    use time::{format_description::well_known::Rfc3339, OffsetDateTime};
    OffsetDateTime::parse(s, &Rfc3339)
        .map(|_| Since::Wall(s.to_string()))
        .map_err(|e| anyhow!("--since {s:?} not a duration (30s/5m/2h/1d) or RFC3339: {e}"))
}

fn parse_duration(s: &str) -> Option<u64> {
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    let (num_str, unit_str) = s.split_at(s.len().saturating_sub(1));
    let n: u64 = num_str.parse().ok()?;
    let ns = match unit_str {
        "s" => n * 1_000_000_000,
        "m" => n * 60 * 1_000_000_000,
        "h" => n * 3600 * 1_000_000_000,
        "d" => n * 86400 * 1_000_000_000,
        _ => return None,
    };
    Some(ns)
}

fn event_is_after(ev: &serde_json::Value, since: &Since) -> bool {
    match since {
        Since::HostNs(floor) => ev
            .get("host_ts_ns")
            .and_then(|v| v.as_u64())
            .map(|ns| ns >= *floor)
            .unwrap_or(false),
        Since::Wall(floor_rfc3339) => ev
            .get("@timestamp")
            .and_then(|v| v.as_str())
            .map(|ts| ts >= floor_rfc3339.as_str())
            .unwrap_or(false),
    }
}

fn follow_events(
    path: &std::path::Path,
    filters: &[FilterExpr],
    since: Option<&Since>,
) -> Result<()> {
    use std::io::{BufRead, BufReader, Seek};
    let f = std::fs::File::open(path)
        .with_context(|| format!("reopen events for follow: {}", path.display()))?;
    let mut reader = BufReader::new(f);
    reader.seek(std::io::SeekFrom::End(0)).ok();
    loop {
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) => std::thread::sleep(std::time::Duration::from_millis(200)),
            Ok(_) => {
                let trimmed = line.trim_end_matches('\n');
                if trimmed.is_empty() {
                    continue;
                }
                let parsed: serde_json::Value = match serde_json::from_str(trimmed) {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                if !event_matches(&parsed, filters) {
                    continue;
                }
                if let Some(since) = since {
                    if !event_is_after(&parsed, since) {
                        continue;
                    }
                }
                print!("{line}");
            }
            Err(e) => return Err(e.into()),
        }
    }
}

/// `umlctl assert` dispatch. Reads the resolved bundle's
/// events.jsonl and evaluates every `--no-*` / `--deny` /
/// `--require` predicate. On any violation, writes a report
/// to stderr and exits with code 1; on pass, prints a one-
/// line OK summary (unless --quiet) and returns normally.
///
/// Designed so a kselftest can replace dmesg-grep with
/// `umlctl assert $RUN --no-kasan --no-kcsan --no-panic`
/// and get a structured pass/fail + counts + sample
/// offending events — no more fragile regexes.
pub fn cmd_assert(paths: &Paths, args: &AssertArgs, quiet: bool) -> Result<()> {
    let run_id = resolve_name_or_run_id(paths, &args.name_or_run_id);
    let events_path = paths.run_dir(&run_id).join("events.jsonl");
    if !events_path.exists() {
        eprintln!("umlctl: events.jsonl missing for run {}", run_id);
        std::process::exit(7);
    }

    // Read everything into memory. Bundle sizes are bounded by
    // the producer (ring overflow in later phases emits gap
    // markers instead of growing unbounded); it's fine to
    // slurp here.
    let content = std::fs::read_to_string(&events_path)
        .with_context(|| format!("read {}", events_path.display()))?;

    // Build predicate list.
    let mut denies: Vec<String> = Vec::new();
    let mut requires: Vec<String> = Vec::new();

    if args.no_panic {
        denies.push("uml.panic.v1".into());
    }
    if args.no_oom {
        denies.push("uml.oom.v1".into());
    }
    if args.no_kasan {
        denies.push("uml.sanitizer.kasan.v1".into());
    }
    if args.no_kcsan {
        denies.push("uml.sanitizer.kcsan.v1".into());
    }
    if args.no_kmsan {
        denies.push("uml.sanitizer.kmsan.v1".into());
    }
    if args.no_kfence {
        denies.push("uml.sanitizer.kfence.v1".into());
    }
    if args.no_ubsan {
        denies.push("uml.sanitizer.ubsan.v1".into());
    }
    if args.no_rcu_stall {
        denies.push("uml.rcu_stall.v1".into());
    }
    if args.no_lockdep {
        denies.push("uml.lockdep.v1".into());
    }
    if args.no_watchdog_stall {
        denies.push("uml.watchdog_stall.v1".into());
    }

    // Additional --deny / --require predicates. Preserve
    // user order so the violation report matches invocation
    // order.
    denies.extend(args.deny.iter().cloned());
    requires.extend(args.require.iter().cloned());

    if denies.is_empty() && requires.is_empty() {
        eprintln!("umlctl: assert needs at least one predicate (--no-*, --deny, or --require)");
        std::process::exit(2);
    }

    // Tally per-schema. Pre-seed zero counts for --require
    // schemas so "zero observed" registers.
    let mut counts: std::collections::HashMap<String, Vec<String>> =
        std::collections::HashMap::new();
    for s in denies.iter().chain(requires.iter()) {
        counts.entry(s.clone()).or_default();
    }

    let mut total_events = 0usize;
    for line in content.lines() {
        if line.trim().is_empty() {
            continue;
        }
        total_events += 1;
        let Ok(parsed): serde_json::Result<serde_json::Value> = serde_json::from_str(line) else {
            continue;
        };
        let schema = parsed
            .get("schema")
            .and_then(|v| v.as_str())
            .unwrap_or_default();
        if let Some(bucket) = counts.get_mut(schema) {
            // Cap the sample per schema; full list wasteful in
            // CI logs when a kernel spews 500 KASAN reports.
            if bucket.len() < 3 {
                bucket.push(line.to_string());
            } else {
                bucket.push(String::new()); // Count-only marker.
            }
        }
    }

    // Evaluate predicates.
    let mut violations: Vec<String> = Vec::new();
    for s in &denies {
        let hits = counts.get(s).map(Vec::len).unwrap_or(0);
        if hits > 0 {
            let samples = counts.get(s).unwrap();
            let sample_lines: Vec<&str> = samples
                .iter()
                .filter(|l| !l.is_empty())
                .map(String::as_str)
                .collect();
            let mut msg = format!("  deny[{s}]: {hits} event(s)");
            for sample in sample_lines.iter().take(3) {
                msg.push_str(&format!("\n    {sample}"));
            }
            if hits > 3 {
                msg.push_str(&format!("\n    ... + {} more suppressed", hits - 3));
            }
            violations.push(msg);
        }
    }
    for s in &requires {
        let hits = counts.get(s).map(Vec::len).unwrap_or(0);
        if hits == 0 {
            violations.push(format!("  require[{s}]: 0 events (expected >= 1)"));
        }
    }

    if violations.is_empty() {
        if !quiet {
            println!(
                "umlctl assert: OK (run {run_id}, {} event(s), {} predicate(s))",
                total_events,
                denies.len() + requires.len()
            );
        }
        return Ok(());
    }

    eprintln!(
        "umlctl assert FAILED for run {run_id} ({} event(s), {} violation(s))",
        total_events,
        violations.len()
    );
    for v in &violations {
        eprintln!("{v}");
    }
    std::process::exit(1);
}

fn resolve_name_or_run_id(paths: &Paths, raw: &str) -> String {
    let is_ulid = raw.len() == 26
        && raw
            .chars()
            .all(|c| c.is_ascii_digit() || (c.is_ascii_uppercase() && c != 'I' && c != 'L' && c != 'O' && c != 'U'));
    if is_ulid {
        return raw.to_string();
    }
    let manifest_path = paths.manifest_path(raw);
    if !manifest_path.exists() {
        eprintln!("umlctl: instance '{}' not found", raw);
        std::process::exit(3);
    }
    let live = super::supervise::read_run_id_file(&paths.run_id_file_path(raw));
    match live.or_else(|| run::latest_run_for(paths, raw)) {
        Some(id) => id,
        None => {
            eprintln!("umlctl: no events bundle for instance '{}'", raw);
            std::process::exit(7);
        }
    }
}

/// Convenience for the umlctl lifecycle emissions
/// (`uml.lifecycle.v1`). Action is `create` | `start` | `stop`
/// | `rm`; `pid` + `signal` are optional per action.
pub fn emit_lifecycle(
    paths: &Paths,
    run_id: &str,
    instance: &str,
    action: &str,
    pid: Option<u32>,
    signal: Option<&str>,
    exit_status: Option<i32>,
) -> Result<()> {
    let mut extra = serde_json::Map::new();
    if let Some(p) = pid {
        extra.insert("pid".into(), serde_json::Value::from(p));
    }
    if let Some(s) = signal {
        extra.insert("signal".into(), serde_json::Value::from(s));
    }
    if let Some(es) = exit_status {
        extra.insert("exit_status".into(), serde_json::Value::from(es));
    }
    emit(
        paths,
        run_id,
        Event {
            schema: "uml.lifecycle.v1",
            category: "lifecycle",
            severity: Severity::Info,
            action,
            instance,
            extra: if extra.is_empty() {
                None
            } else {
                Some(serde_json::Value::Object(extra))
            },
        },
    )
}
