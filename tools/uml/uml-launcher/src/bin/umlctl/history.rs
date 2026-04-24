// SPDX-License-Identifier: GPL-2.0
//
// Append-only audit log for umlctl.
//
// One JSON object per line under $STATE/history.jsonl. The
// format stays deliberately flat (ts/event/name + optional
// pid/exit_status/signal_sent) so post-mortem tooling can
// grep it without a parser. History is for after-the-fact
// reconstruction; `ps` is for current state.

use anyhow::{Context, Result};
use serde::Serialize;
use std::io::Write;

use super::manifest;
use super::paths::Paths;

#[derive(Debug)]
pub enum Event<'a> {
    Create {
        name: &'a str,
    },
    Start {
        name: &'a str,
        pid: u32,
    },
    Stop {
        name: &'a str,
        pid: u32,
        exit_status: Option<i32>,
        signal_sent: &'a str,
    },
    Rm {
        name: &'a str,
    },
}

#[derive(Serialize)]
struct Record<'a> {
    ts: String,
    event: &'static str,
    name: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pid: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    exit_status: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    signal_sent: Option<&'a str>,
}

pub fn append(paths: &Paths, event: Event<'_>) -> Result<()> {
    let ts = manifest::now_rfc3339();
    let record = match event {
        Event::Create { name } => Record {
            ts,
            event: "create",
            name,
            pid: None,
            exit_status: None,
            signal_sent: None,
        },
        Event::Start { name, pid } => Record {
            ts,
            event: "start",
            name,
            pid: Some(pid),
            exit_status: None,
            signal_sent: None,
        },
        Event::Stop {
            name,
            pid,
            exit_status,
            signal_sent,
        } => Record {
            ts,
            event: "stop",
            name,
            pid: Some(pid),
            exit_status,
            signal_sent: Some(signal_sent),
        },
        Event::Rm { name } => Record {
            ts,
            event: "rm",
            name,
            pid: None,
            exit_status: None,
            signal_sent: None,
        },
    };

    let line = serde_json::to_string(&record).context("serialize history record")?;
    let path = paths.history_path();
    let mut f = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .with_context(|| format!("open history file {}", path.display()))?;
    writeln!(f, "{line}").context("write history record")?;
    Ok(())
}
