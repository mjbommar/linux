// SPDX-License-Identifier: GPL-2.0
//
// `umlctl ps` / registry listing.
//
// Scans $STATE/instances/*.toml, joins each manifest against
// the pidfile + /proc/<pid>/ for liveness and cheap stats, and
// renders human table or NDJSON. Filters evaluate left-to-right
// AND. Matches the docker/podman `ps` shape without the daemon.

use anyhow::{anyhow, Context, Result};
use serde::Serialize;
use std::collections::BTreeMap;
use std::io::Write;

use super::manifest::Manifest;
use super::paths::Paths;
use super::supervise;
use super::PsArgs;

#[derive(Serialize, Debug)]
pub struct Row {
    pub name: String,
    pub pid: Option<u32>,
    pub backend: Option<String>,
    pub profile: String,
    pub mem: String,
    pub uptime_secs: Option<u64>,
    pub state: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rss_kb: Option<u64>,
    pub labels: BTreeMap<String, String>,
}

pub fn list(paths: &Paths, args: &PsArgs) -> Result<Vec<Row>> {
    let filters = parse_filters(&args.filters)?;
    let dir = paths.instances_dir();
    let mut rows = Vec::new();

    if !dir.exists() {
        return Ok(rows);
    }

    for entry in std::fs::read_dir(&dir).with_context(|| format!("read {}", dir.display()))? {
        let entry = entry?;
        let p = entry.path();
        if p.extension().and_then(|s| s.to_str()) != Some("toml") {
            continue;
        }
        // Skip unreadable manifests rather than aborting the
        // whole listing — one bad file shouldn't break `ps`.
        let m = match Manifest::read(&p) {
            Ok(m) => m,
            Err(_) => continue,
        };

        let pidfile = paths.pidfile_path(&m.instance.name);
        let ident = supervise::read_pidfile(&pidfile);
        let alive = match ident {
            Some(i) => supervise::identity_alive(i),
            None => false,
        };
        let state: &'static str = if alive { "running" } else { "stopped" };

        if !args.all && !alive {
            continue;
        }

        let pid = ident.map(|i| i.pid);
        let uptime_secs = if alive {
            pid.and_then(process_uptime)
        } else {
            None
        };

        let rss_kb = if args.size && alive {
            pid.and_then(process_rss_kb)
        } else {
            None
        };

        let row = Row {
            name: m.instance.name.clone(),
            pid: if alive { pid } else { None },
            backend: if alive {
                Some(m.kernel.backend.clone())
            } else {
                None
            },
            profile: m.kernel.profile.clone(),
            mem: m.runtime.mem.clone(),
            uptime_secs,
            state,
            rss_kb,
            labels: m.labels.clone(),
        };

        if !row_matches_filters(&row, &filters) {
            continue;
        }
        rows.push(row);
    }

    rows.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(rows)
}

struct Filter {
    key: String,
    value: String,
}

fn parse_filters(raw: &[String]) -> Result<Vec<Filter>> {
    let mut out = Vec::new();
    for s in raw {
        let (k, v) = s
            .split_once('=')
            .ok_or_else(|| anyhow!("filter must be K=V, got {s:?}"))?;
        out.push(Filter {
            key: k.to_string(),
            value: v.to_string(),
        });
    }
    Ok(out)
}

fn row_matches_filters(row: &Row, filters: &[Filter]) -> bool {
    for f in filters {
        let ok = match f.key.as_str() {
            "state" => row.state == f.value,
            "profile" => row.profile == f.value,
            "backend" => row.backend.as_deref() == Some(f.value.as_str()),
            "name" => row.name == f.value,
            "label" => {
                // label=k=v — split v on the first '='.
                let Some((lk, lv)) = f.value.split_once('=') else {
                    return false;
                };
                row.labels.get(lk).map(String::as_str) == Some(lv)
            }
            _ => return false,
        };
        if !ok {
            return false;
        }
    }
    true
}

pub fn print_table(rows: &[Row], args: &PsArgs) -> Result<()> {
    if args.quiet {
        for r in rows {
            println!("{}", r.name);
        }
        return Ok(());
    }

    if args.size {
        println!(
            "{:<20} {:<8} {:<9} {:<12} {:<6} {:<10} {:<9} STATE",
            "NAME", "PID", "BACKEND", "PROFILE", "MEM", "UPTIME", "RSS(kB)"
        );
    } else {
        println!(
            "{:<20} {:<8} {:<9} {:<12} {:<6} {:<10} STATE",
            "NAME", "PID", "BACKEND", "PROFILE", "MEM", "UPTIME"
        );
    }

    for r in rows {
        let pid_s = r.pid.map(|p| p.to_string()).unwrap_or_else(|| "-".into());
        let backend_s = r.backend.clone().unwrap_or_else(|| "-".into());
        let uptime_s = r
            .uptime_secs
            .map(format_uptime)
            .unwrap_or_else(|| "-".into());
        if args.size {
            let rss_s = r
                .rss_kb
                .map(|k| k.to_string())
                .unwrap_or_else(|| "-".into());
            println!(
                "{:<20} {:<8} {:<9} {:<12} {:<6} {:<10} {:<9} {}",
                r.name, pid_s, backend_s, r.profile, r.mem, uptime_s, rss_s, r.state
            );
        } else {
            println!(
                "{:<20} {:<8} {:<9} {:<12} {:<6} {:<10} {}",
                r.name, pid_s, backend_s, r.profile, r.mem, uptime_s, r.state
            );
        }
    }
    Ok(())
}

pub fn print_json(rows: &[Row]) -> Result<()> {
    let stdout = std::io::stdout();
    let mut lock = stdout.lock();
    for r in rows {
        let line = serde_json::to_string(r).context("serialize row")?;
        writeln!(lock, "{line}")?;
    }
    Ok(())
}

fn format_uptime(secs: u64) -> String {
    if secs < 60 {
        format!("{secs}s")
    } else if secs < 3600 {
        format!("{:02}:{:02}m", secs / 60, secs % 60)
    } else {
        format!("{}h{:02}m", secs / 3600, (secs % 3600) / 60)
    }
}

fn process_uptime(pid: u32) -> Option<u64> {
    // /proc/<pid>/stat field 22 (starttime, in clock ticks since
    // boot), joined against /proc/uptime's first value (host
    // uptime seconds). The comm field (#2) can contain spaces
    // and parens — scan to the last ')' before tokenizing.
    let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let rparen = stat.rfind(')')?;
    let rest = stat.get(rparen + 1..)?.trim_start();
    let fields: Vec<&str> = rest.split_whitespace().collect();
    // After the comm, field 3 (= post-comm index 0) is state.
    // Field 22 (= post-comm index 19) is starttime.
    let starttime_ticks: u64 = fields.get(19)?.parse().ok()?;
    let uptime = std::fs::read_to_string("/proc/uptime").ok()?;
    let uptime_secs: f64 = uptime.split_whitespace().next()?.parse().ok()?;
    let ticks_per_sec = unsafe { libc::sysconf(libc::_SC_CLK_TCK) } as f64;
    if ticks_per_sec <= 0.0 {
        return None;
    }
    let start_secs = starttime_ticks as f64 / ticks_per_sec;
    Some((uptime_secs - start_secs).max(0.0) as u64)
}

fn process_rss_kb(pid: u32) -> Option<u64> {
    // /proc/<pid>/statm: "size resident shared text lib data dt"
    // in pages. Multiply resident by the page size in kB.
    let s = std::fs::read_to_string(format!("/proc/{pid}/statm")).ok()?;
    let mut it = s.split_whitespace();
    let _size = it.next()?;
    let resident: u64 = it.next()?.parse().ok()?;
    let page_kb = unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as u64 / 1024;
    Some(resident * page_kb)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(name: &str, state: &'static str, profile: &str, backend: Option<&str>) -> Row {
        Row {
            name: name.into(),
            pid: None,
            backend: backend.map(String::from),
            profile: profile.into(),
            mem: "256M".into(),
            uptime_secs: None,
            state,
            rss_kb: None,
            labels: Default::default(),
        }
    }

    #[test]
    fn filter_state_and_profile() {
        let r = row("a", "running", "research", Some("seccomp"));
        let f = parse_filters(&["state=running".into(), "profile=research".into()]).unwrap();
        assert!(row_matches_filters(&r, &f));
        let f2 = parse_filters(&["state=stopped".into()]).unwrap();
        assert!(!row_matches_filters(&r, &f2));
    }

    #[test]
    fn filter_label() {
        let mut r = row("a", "running", "research", Some("seccomp"));
        r.labels.insert("env".into(), "prod".into());
        let f = parse_filters(&["label=env=prod".into()]).unwrap();
        assert!(row_matches_filters(&r, &f));
        let f2 = parse_filters(&["label=env=dev".into()]).unwrap();
        assert!(!row_matches_filters(&r, &f2));
    }

    #[test]
    fn format_uptime_boundaries() {
        assert_eq!(format_uptime(5), "5s");
        assert_eq!(format_uptime(65), "01:05m");
        assert_eq!(format_uptime(3700), "1h01m");
    }

    #[test]
    fn bad_filter_rejected() {
        assert!(parse_filters(&["no-equals".into()]).is_err());
    }
}
