// SPDX-License-Identifier: GPL-2.0
//
// dmesg_parse — convert kernel printk splats in `kernel.log`
// into structured events.jsonl records.
//
// This is a transitional path: the upstream direction is to
// add tracepoints inside each sanitizer's report path so
// native event emission replaces the parser. Until that
// lands, selftests that want "no KASAN hit" assertions need a
// producer — and dmesg is the only producer we have today.
//
// The parser runs at `umlctl stop` time, right after
// `console_split::derive_kernel_log()` materializes
// `kernel.log`. One splat line per `emit()` call → one JSON
// object per line in `events.jsonl`. `umlctl assert
// --no-kasan` etc. already know which schema to look for, so
// producing real events here makes `assert` work on
// real UML output, not just synthetic injection.
//
// Conservative classifier: only matches on the specific
// leading tokens each subsystem uses in its BUG/WARN splat.
// False-positives on user-printed text would poison the
// events.jsonl stream, so the patterns stay tight and
// boot-time banners (e.g. "KASAN enabled") are explicitly
// NOT matched.

use anyhow::Result;
use serde_json::json;

use super::events::{emit, Event, Severity};
use super::paths::Paths;

/// Every splat kind we know how to recognize, paired with the
/// schema-name we emit for it. Keep in sync with the schema
/// registry (`schema.rs`) and the `--no-*` predicate aliases
/// in `events.rs::cmd_assert`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SplatKind {
    Kasan,
    Kfence,
    Kcsan,
    Kmsan,
    Ubsan,
    Panic,
    Oom,
    RcuStall,
    Lockdep,
    WatchdogStall,
}

impl SplatKind {
    pub fn schema(self) -> &'static str {
        match self {
            SplatKind::Kasan => "uml.sanitizer.kasan.v1",
            SplatKind::Kfence => "uml.sanitizer.kfence.v1",
            SplatKind::Kcsan => "uml.sanitizer.kcsan.v1",
            SplatKind::Kmsan => "uml.sanitizer.kmsan.v1",
            SplatKind::Ubsan => "uml.sanitizer.ubsan.v1",
            SplatKind::Panic => "uml.panic.v1",
            SplatKind::Oom => "uml.oom.v1",
            SplatKind::RcuStall => "uml.rcu_stall.v1",
            SplatKind::Lockdep => "uml.lockdep.v1",
            SplatKind::WatchdogStall => "uml.watchdog_stall.v1",
        }
    }

    pub fn category(self) -> &'static str {
        match self {
            SplatKind::Kasan
            | SplatKind::Kfence
            | SplatKind::Kcsan
            | SplatKind::Kmsan
            | SplatKind::Ubsan => "sanitizer",
            SplatKind::Panic => "crash",
            SplatKind::Oom => "resource",
            SplatKind::RcuStall | SplatKind::WatchdogStall => "stall",
            SplatKind::Lockdep => "lockdep",
        }
    }

    pub fn severity(self) -> Severity {
        match self {
            SplatKind::Panic
            | SplatKind::Kasan
            | SplatKind::Kfence
            | SplatKind::Kcsan
            | SplatKind::Kmsan
            | SplatKind::Ubsan => Severity::Error,
            SplatKind::Oom
            | SplatKind::RcuStall
            | SplatKind::Lockdep
            | SplatKind::WatchdogStall => Severity::Warning,
        }
    }

    /// Short action verb that `umlctl events --filter
    /// event.action=…` can target. `detect` keeps the splat
    /// distinguishable from lifecycle records without forcing
    /// parsers to guess a sub-event.
    pub fn action(self) -> &'static str {
        "detect"
    }
}

/// Drop a leading `[<ts>] ` PRINTK_TIME prefix if present,
/// plus a leading `<N>` priority byte. Returns the payload
/// portion of the line — the part a human cares about.
fn strip_printk_prefix(line: &str) -> &str {
    let mut rest = line;
    if let Some(stripped) = rest.strip_prefix('<') {
        if let Some(end) = stripped.find('>') {
            let inner = &stripped[..end];
            if !inner.is_empty() && inner.chars().all(|c| c.is_ascii_digit()) {
                rest = &stripped[end + 1..];
            }
        }
    }
    if let Some(stripped) = rest.strip_prefix('[') {
        if let Some(end) = stripped.find(']') {
            let inner = &stripped[..end];
            let trimmed = inner.trim_start();
            if let Some((secs, usecs)) = trimmed.split_once('.') {
                let seconds_ok = !secs.is_empty() && secs.chars().all(|c| c.is_ascii_digit());
                let usecs_ok = !usecs.is_empty() && usecs.chars().all(|c| c.is_ascii_digit());
                if seconds_ok && usecs_ok {
                    rest = stripped[end + 1..].trim_start();
                }
            }
        }
    }
    rest
}

/// Classify a single payload line (post-prefix) into a splat
/// kind, or None for "not interesting." Matches are leading-
/// token only: "BUG: KASAN:", "BUG: KFENCE:", etc.
pub fn classify_line(payload: &str) -> Option<SplatKind> {
    // Sanitizers all wear `BUG: <NAME>:` prefixes. Matching
    // on the leading token rejects user-space echoes that
    // happen to contain the word KASAN mid-string.
    if let Some(rest) = payload.strip_prefix("BUG: ") {
        if rest.starts_with("KASAN:") {
            return Some(SplatKind::Kasan);
        }
        if rest.starts_with("KFENCE:") {
            return Some(SplatKind::Kfence);
        }
        if rest.starts_with("KCSAN:") {
            return Some(SplatKind::Kcsan);
        }
        if rest.starts_with("KMSAN:") {
            return Some(SplatKind::Kmsan);
        }
    }
    if payload.starts_with("UBSAN:") {
        return Some(SplatKind::Ubsan);
    }
    // `Kernel panic - not syncing: ...` is the canonical
    // panic() entry. `Panic:` alone is too permissive.
    if payload.starts_with("Kernel panic - not syncing:") || payload.starts_with("Kernel panic:") {
        return Some(SplatKind::Panic);
    }
    // OOM has two entry shapes depending on whether the killer
    // was triggered from a cgroup or globally.
    if payload.starts_with("Out of memory:")
        || payload.starts_with("oom-kill:")
        || payload.starts_with("oom_reaper:")
    {
        return Some(SplatKind::Oom);
    }
    // RCU stalls print `rcu: INFO: rcu_sched detected stalls` or
    // `INFO: rcu_sched self-detected stall`.
    if payload.starts_with("rcu: INFO:") && payload.contains("stall")
        || payload.starts_with("INFO: rcu_") && payload.contains("stall")
    {
        return Some(SplatKind::RcuStall);
    }
    // Lockdep's WARN rendering starts with "WARNING:" plus
    // one of several recursive/ordering markers, OR a
    // lockdep-possible line.
    if payload.starts_with("WARNING: possible recursive locking detected")
        || payload.starts_with("WARNING: possible circular locking dependency detected")
        || payload.starts_with("WARNING: inconsistent lock state")
        || payload.starts_with("INFO: possible recursive locking detected")
    {
        return Some(SplatKind::Lockdep);
    }
    // hardlockup / softlockup / watchdog timestamps.
    if payload.starts_with("watchdog: BUG: soft lockup")
        || payload.starts_with("Watchdog detected hard LOCKUP")
        || payload.starts_with("NMI watchdog:")
    {
        return Some(SplatKind::WatchdogStall);
    }
    None
}

/// Extract the free-form message portion after the leading
/// token. Used for the `message` field in the emitted event
/// body so users can see what matched without re-reading kernel.log.
fn extract_message(kind: SplatKind, payload: &str) -> String {
    let body = match kind {
        SplatKind::Kasan | SplatKind::Kfence | SplatKind::Kcsan | SplatKind::Kmsan => {
            // "BUG: KASAN: use-after-free in …" → "use-after-free in …"
            payload
                .splitn(3, ':')
                .nth(2)
                .map(|s| s.trim_start())
                .unwrap_or(payload)
        }
        SplatKind::Ubsan => payload
            .strip_prefix("UBSAN:")
            .unwrap_or(payload)
            .trim_start(),
        SplatKind::Panic => payload
            .strip_prefix("Kernel panic - not syncing:")
            .or_else(|| payload.strip_prefix("Kernel panic:"))
            .unwrap_or(payload)
            .trim_start(),
        SplatKind::Oom => payload,
        SplatKind::RcuStall => payload,
        SplatKind::Lockdep => payload,
        SplatKind::WatchdogStall => payload,
    };
    body.trim().to_string()
}

/// Parse `kernel_log` for splats and emit one event per match
/// into `events.jsonl` for `run_id`. Best-effort: a missing
/// kernel.log is not an error (nothing to do); I/O failures on
/// individual lines are skipped rather than poisoning the stop
/// path.
///
/// Returns the number of events emitted.
pub fn parse_and_emit(
    paths: &Paths,
    run_id: &str,
    instance: &str,
    kernel_log: &std::path::Path,
) -> Result<usize> {
    if !kernel_log.exists() {
        return Ok(0);
    }
    let content = match std::fs::read_to_string(kernel_log) {
        Ok(s) => s,
        Err(_) => return Ok(0),
    };
    let mut emitted = 0usize;
    for line in content.lines() {
        let payload = strip_printk_prefix(line);
        let Some(kind) = classify_line(payload) else {
            continue;
        };
        let message = extract_message(kind, payload);
        let extra = json!({ "message": message });
        emit(
            paths,
            run_id,
            Event {
                schema: kind.schema(),
                category: kind.category(),
                severity: kind.severity(),
                action: kind.action(),
                instance,
                extra: Some(extra),
            },
        )?;
        emitted += 1;
    }
    Ok(emitted)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strip_timestamp_prefix() {
        assert_eq!(strip_printk_prefix("[    0.123456] Hello"), "Hello");
        assert_eq!(strip_printk_prefix("[1.0] X"), "X");
    }

    #[test]
    fn strip_priority_and_timestamp() {
        assert_eq!(strip_printk_prefix("<4>[    0.123] hello"), "hello");
        assert_eq!(strip_printk_prefix("<3>oops"), "oops");
    }

    #[test]
    fn strip_leaves_non_printk_alone() {
        assert_eq!(strip_printk_prefix("$ echo hi"), "$ echo hi");
        assert_eq!(strip_printk_prefix("[info] userland"), "[info] userland");
    }

    #[test]
    fn classify_sanitizers() {
        assert_eq!(
            classify_line("BUG: KASAN: use-after-free in foo+0x1/0x2"),
            Some(SplatKind::Kasan)
        );
        assert_eq!(
            classify_line("BUG: KFENCE: out-of-bounds read in bar"),
            Some(SplatKind::Kfence)
        );
        assert_eq!(
            classify_line("BUG: KCSAN: data-race in baz"),
            Some(SplatKind::Kcsan)
        );
        assert_eq!(
            classify_line("BUG: KMSAN: uninit-value in qux"),
            Some(SplatKind::Kmsan)
        );
        assert_eq!(
            classify_line("UBSAN: shift-out-of-bounds in foo.c:12:4"),
            Some(SplatKind::Ubsan)
        );
    }

    #[test]
    fn classify_panic_oom_stall() {
        assert_eq!(
            classify_line("Kernel panic - not syncing: bad thing"),
            Some(SplatKind::Panic)
        );
        assert_eq!(
            classify_line("Out of memory: Killed process 123 (foo)"),
            Some(SplatKind::Oom)
        );
        assert_eq!(
            classify_line("oom-kill: constraint=... cpus_allowed=..."),
            Some(SplatKind::Oom)
        );
        assert_eq!(
            classify_line("rcu: INFO: rcu_sched detected stalls on CPUs"),
            Some(SplatKind::RcuStall)
        );
        assert_eq!(
            classify_line("INFO: rcu_sched self-detected stall on CPU"),
            Some(SplatKind::RcuStall)
        );
    }

    #[test]
    fn classify_lockdep_watchdog() {
        assert_eq!(
            classify_line("WARNING: possible recursive locking detected"),
            Some(SplatKind::Lockdep)
        );
        assert_eq!(
            classify_line("WARNING: possible circular locking dependency detected"),
            Some(SplatKind::Lockdep)
        );
        assert_eq!(
            classify_line("watchdog: BUG: soft lockup - CPU#0 stuck for 22s!"),
            Some(SplatKind::WatchdogStall)
        );
        assert_eq!(
            classify_line("Watchdog detected hard LOCKUP on cpu 1"),
            Some(SplatKind::WatchdogStall)
        );
        assert_eq!(
            classify_line("NMI watchdog: Watchdog detected hard LOCKUP"),
            Some(SplatKind::WatchdogStall)
        );
    }

    #[test]
    fn rejects_boot_banners_and_userspace_echoes() {
        // Boot-time "KASAN enabled" banner should NOT be flagged
        // as an event — that's a configuration log, not a bug.
        assert_eq!(classify_line("KASAN: enabled, 256 bytes"), None);
        // User typing "BUG: KASAN" in a shell shouldn't match
        // because the line would lack the printk prefix that
        // console_split already filtered for.
        assert_eq!(classify_line("echo 'BUG: KASAN: pretend'"), None);
        // UBSAN docstring without the colon.
        assert_eq!(classify_line("UBSAN loaded"), None);
        // Generic warning lines.
        assert_eq!(classify_line("WARNING: CPU at some address"), None);
    }

    #[test]
    fn extract_message_trims_prefix() {
        let m = extract_message(
            SplatKind::Kasan,
            "BUG: KASAN: use-after-free in foo+0x1/0x2",
        );
        assert_eq!(m, "use-after-free in foo+0x1/0x2");
        let m = extract_message(
            SplatKind::Panic,
            "Kernel panic - not syncing: something bad",
        );
        assert_eq!(m, "something bad");
        let m = extract_message(SplatKind::Ubsan, "UBSAN: shift-out-of-bounds");
        assert_eq!(m, "shift-out-of-bounds");
    }

    #[test]
    fn schema_matches_registry_names() {
        // Every schema the parser produces must appear in the
        // schema registry. This catches the
        // "new splat kind, forgot to declare it" failure mode.
        use super::super::schema::REGISTRY;
        let declared: std::collections::HashSet<&str> = REGISTRY.iter().map(|s| s.name).collect();
        for k in [
            SplatKind::Kasan,
            SplatKind::Kfence,
            SplatKind::Kcsan,
            SplatKind::Kmsan,
            SplatKind::Ubsan,
            SplatKind::Panic,
            SplatKind::Oom,
            SplatKind::RcuStall,
            SplatKind::Lockdep,
            SplatKind::WatchdogStall,
        ] {
            assert!(
                declared.contains(k.schema()),
                "kind {:?} produces schema {} but schema.rs REGISTRY doesn't declare it",
                k,
                k.schema()
            );
        }
    }
}
