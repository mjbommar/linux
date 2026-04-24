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

use anyhow::{Context, Result};
use serde::Serialize;
use std::io::Write;

use super::manifest;
use super::paths::Paths;
use super::run;

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
