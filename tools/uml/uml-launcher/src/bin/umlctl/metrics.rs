// SPDX-License-Identifier: GPL-2.0
//
// metrics — host-side `/proc/<pid>/*` + cgroup v2 scraper for
// `umlctl metrics`. Phase O2 of the observability spine
// (Documentation/virt/uml/redesign/08-future-phases/
// 13-uml-observability-spine.md).
//
// One-shot scrape (no daemon, no listener): resolves the
// instance's live pid via the runtime pidfile, reads a
// curated slice of `/proc/<pid>/*` and `/sys/fs/cgroup/<path>/*`,
// and renders it human-readable or as a single JSON blob.
//
// Deliberate non-goals for v1:
//   - no OpenMetrics HTTP endpoint (would need a daemon).
//   - no eBPF off-CPU/wakeup-lat scraping (bigger lift, its
//     own phase).
//   - no sampling loop (callers can `watch umlctl metrics ...`).
//
// The fields chosen are the ones that matter for UML
// post-mortem: memory footprint (RSS, VmSize, cgroup
// memory.current), CPU time (utime/stime + cgroup
// cpu.stat), scheduling (nr_voluntary_switches,
// nr_involuntary_switches, schedstat run_time_ns), IO
// (read_bytes, write_bytes), and liveness indicators
// (state, threads). Everything else is parking-lot.

use anyhow::{Context, Result};
use serde::Serialize;
use std::path::Path;

#[derive(Serialize, Debug, Default, Clone)]
pub struct ProcMetrics {
    pub pid: u32,
    pub name: String,
    pub state: String,
    pub threads: u32,
    pub vm_size_bytes: u64,
    pub vm_rss_bytes: u64,
    pub vm_peak_bytes: u64,
    pub vm_swap_bytes: u64,
    /// User-mode CPU time, clock ticks.
    pub utime_ticks: u64,
    /// Kernel-mode CPU time, clock ticks.
    pub stime_ticks: u64,
    pub start_time_ticks: u64,
    pub nr_voluntary_ctxt_switches: u64,
    pub nr_involuntary_ctxt_switches: u64,
    /// Read/written through read(2)/write(2) — syscall-level.
    pub read_chars: u64,
    pub write_chars: u64,
    /// Read/written to the storage layer (cancelled_write_bytes
    /// subtracted).
    pub read_bytes: u64,
    pub write_bytes: u64,
    /// /proc/<pid>/schedstat fields: run_time_ns, wait_time_ns, pcount.
    pub sched_run_ns: u64,
    pub sched_wait_ns: u64,
    pub sched_pcount: u64,
    /// Clock tick rate (Hz) — so a consumer can convert ticks
    /// to seconds without a second syscall.
    pub clock_hz: u32,
}

#[derive(Serialize, Debug, Default, Clone)]
pub struct CgroupMetrics {
    /// Resolved cgroup path (inside the unified v2 hierarchy).
    /// Empty when we can't resolve (no v2, or non-cgroup-delegated
    /// setup).
    pub path: String,
    pub memory_current_bytes: Option<u64>,
    pub memory_peak_bytes: Option<u64>,
    pub memory_swap_current_bytes: Option<u64>,
    /// cpu.stat fields (µs): usage, user, system.
    pub cpu_usage_usec: Option<u64>,
    pub cpu_user_usec: Option<u64>,
    pub cpu_system_usec: Option<u64>,
    pub pids_current: Option<u64>,
    pub pids_max: Option<String>,
}

#[derive(Serialize, Debug, Default, Clone)]
pub struct Metrics {
    pub proc: ProcMetrics,
    pub cgroup: CgroupMetrics,
}

/// Scrape everything we can for `pid`. Each individual field
/// is best-effort: a missing `/proc` file (process gone mid-
/// scrape) leaves the corresponding field at its default
/// rather than failing the whole call. A totally-absent pid
/// dir still surfaces as an error so the caller can tell
/// "process exited" from "process has no io accounting."
pub fn scrape(pid: u32) -> Result<Metrics> {
    let base = format!("/proc/{pid}");
    if !Path::new(&base).exists() {
        anyhow::bail!("process {pid} not found (exited?)");
    }
    let mut m = Metrics::default();
    m.proc.pid = pid;
    m.proc.clock_hz = clock_tick_hz();

    if let Ok(s) = std::fs::read_to_string(format!("{base}/status")) {
        parse_status(&s, &mut m.proc);
    }
    if let Ok(s) = std::fs::read_to_string(format!("{base}/stat")) {
        parse_stat(&s, &mut m.proc);
    }
    if let Ok(s) = std::fs::read_to_string(format!("{base}/io")) {
        parse_io(&s, &mut m.proc);
    }
    if let Ok(s) = std::fs::read_to_string(format!("{base}/schedstat")) {
        parse_schedstat(&s, &mut m.proc);
    }

    // cgroup v2 best-effort: /proc/<pid>/cgroup lists lines of
    // "hierarchy_id:controllers:path"; unified v2 uses id `0`
    // with an empty controllers field.
    if let Ok(s) = std::fs::read_to_string(format!("{base}/cgroup")) {
        if let Some(path) = parse_cgroup_v2_path(&s) {
            scrape_cgroup(&path, &mut m.cgroup);
        }
    }

    Ok(m)
}

/// Find the `0::<path>` line (unified v2 hierarchy) and return
/// its payload. Returns None on legacy-v1-only systems.
fn parse_cgroup_v2_path(s: &str) -> Option<String> {
    for line in s.lines() {
        // Form: `0::/<path>`
        if let Some(rest) = line.strip_prefix("0::") {
            return Some(rest.trim().to_string());
        }
    }
    None
}

fn scrape_cgroup(rel_path: &str, out: &mut CgroupMetrics) {
    // The mount point is conventionally `/sys/fs/cgroup` on
    // unified-v2 systems; if that's not where v2 lives, we
    // silently emit an empty CgroupMetrics. A production-grade
    // resolver would walk /proc/self/mountinfo, but the
    // conventional mount is accurate on every modern distro.
    let base = format!("/sys/fs/cgroup{rel_path}");
    if !Path::new(&base).exists() {
        return;
    }
    out.path = rel_path.to_string();

    out.memory_current_bytes = read_cgroup_u64(&base, "memory.current");
    out.memory_peak_bytes = read_cgroup_u64(&base, "memory.peak");
    out.memory_swap_current_bytes = read_cgroup_u64(&base, "memory.swap.current");
    out.pids_current = read_cgroup_u64(&base, "pids.current");
    out.pids_max = std::fs::read_to_string(format!("{base}/pids.max"))
        .ok()
        .map(|s| s.trim().to_string());

    if let Ok(s) = std::fs::read_to_string(format!("{base}/cpu.stat")) {
        for line in s.lines() {
            let mut it = line.split_whitespace();
            let (Some(k), Some(v)) = (it.next(), it.next()) else {
                continue;
            };
            let v = v.parse::<u64>().ok();
            match k {
                "usage_usec" => out.cpu_usage_usec = v,
                "user_usec" => out.cpu_user_usec = v,
                "system_usec" => out.cpu_system_usec = v,
                _ => {}
            }
        }
    }
}

fn read_cgroup_u64(base: &str, leaf: &str) -> Option<u64> {
    std::fs::read_to_string(format!("{base}/{leaf}"))
        .ok()
        .and_then(|s| s.trim().parse::<u64>().ok())
}

fn parse_status(s: &str, out: &mut ProcMetrics) {
    for line in s.lines() {
        let Some((k, v)) = line.split_once(':') else {
            continue;
        };
        let v = v.trim();
        match k {
            "Name" => out.name = v.to_string(),
            "State" => out.state = v.to_string(),
            "Threads" => {
                if let Ok(n) = v.parse::<u32>() {
                    out.threads = n;
                }
            }
            // VmRSS / VmSize / VmPeak / VmSwap are "<n> kB".
            "VmSize" => out.vm_size_bytes = parse_kib_field(v),
            "VmRSS" => out.vm_rss_bytes = parse_kib_field(v),
            "VmPeak" => out.vm_peak_bytes = parse_kib_field(v),
            "VmSwap" => out.vm_swap_bytes = parse_kib_field(v),
            "voluntary_ctxt_switches" => {
                if let Ok(n) = v.parse::<u64>() {
                    out.nr_voluntary_ctxt_switches = n;
                }
            }
            "nonvoluntary_ctxt_switches" => {
                if let Ok(n) = v.parse::<u64>() {
                    out.nr_involuntary_ctxt_switches = n;
                }
            }
            _ => {}
        }
    }
}

fn parse_kib_field(v: &str) -> u64 {
    let num: String = v.chars().take_while(|c| c.is_ascii_digit()).collect();
    num.parse::<u64>().unwrap_or(0) * 1024
}

fn parse_stat(s: &str, out: &mut ProcMetrics) {
    // Pid + `(comm)` may contain spaces + close-paren inside
    // comm; the classic parse is to find the last ')' and
    // tokenize after it. man 5 proc enumerates fields by
    // position: after `comm) state` we have: ppid pgrp session
    // tty_nr tpgid flags minflt cminflt majflt cmajflt utime
    // stime cutime cstime priority nice num_threads itrealvalue
    // starttime vsize rss rsslim startcode endcode ...
    let Some(close) = s.rfind(')') else { return };
    let rest = s[close + 1..].trim();
    let fields: Vec<&str> = rest.split_whitespace().collect();
    // Index (relative to after `comm`): utime=11, stime=12,
    // starttime=19 (0-based indexing from "state"=0).
    let get = |i: usize| fields.get(i).and_then(|v| v.parse::<u64>().ok());
    if let Some(utime) = get(11) {
        out.utime_ticks = utime;
    }
    if let Some(stime) = get(12) {
        out.stime_ticks = stime;
    }
    if let Some(start) = get(19) {
        out.start_time_ticks = start;
    }
}

fn parse_io(s: &str, out: &mut ProcMetrics) {
    for line in s.lines() {
        let Some((k, v)) = line.split_once(':') else {
            continue;
        };
        let v = v.trim();
        let Ok(n) = v.parse::<u64>() else { continue };
        match k {
            "rchar" => out.read_chars = n,
            "wchar" => out.write_chars = n,
            "read_bytes" => out.read_bytes = n,
            "write_bytes" => out.write_bytes = n,
            _ => {}
        }
    }
}

fn parse_schedstat(s: &str, out: &mut ProcMetrics) {
    let mut it = s.split_whitespace();
    if let Some(run) = it.next().and_then(|v| v.parse::<u64>().ok()) {
        out.sched_run_ns = run;
    }
    if let Some(wait) = it.next().and_then(|v| v.parse::<u64>().ok()) {
        out.sched_wait_ns = wait;
    }
    if let Some(pc) = it.next().and_then(|v| v.parse::<u64>().ok()) {
        out.sched_pcount = pc;
    }
}

fn clock_tick_hz() -> u32 {
    // SAFETY: sysconf(_SC_CLK_TCK) is always safe and defined
    // per POSIX to return ticks-per-second for the running
    // kernel. Linux glibc returns 100 on the default arch/um
    // config. A negative return is a bug in libc; fall back
    // to 100 in that case.
    let raw = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if raw > 0 {
        raw as u32
    } else {
        100
    }
}

/// Human-readable one-shot rendering of a Metrics blob. Keeps
/// field names close to their /proc counterparts so consumers
/// who already know /proc don't need to re-learn a mapping.
pub fn render_human<W: std::io::Write>(m: &Metrics, mut w: W) -> Result<()> {
    writeln!(w, "pid               {}", m.proc.pid)?;
    writeln!(w, "name              {}", m.proc.name)?;
    writeln!(w, "state             {}", m.proc.state)?;
    writeln!(w, "threads           {}", m.proc.threads)?;
    writeln!(w, "vm_size_bytes     {}", m.proc.vm_size_bytes)?;
    writeln!(w, "vm_rss_bytes      {}", m.proc.vm_rss_bytes)?;
    writeln!(w, "vm_peak_bytes     {}", m.proc.vm_peak_bytes)?;
    writeln!(w, "vm_swap_bytes     {}", m.proc.vm_swap_bytes)?;
    writeln!(
        w,
        "utime_ticks       {}  ({} Hz)",
        m.proc.utime_ticks, m.proc.clock_hz
    )?;
    writeln!(w, "stime_ticks       {}", m.proc.stime_ticks)?;
    writeln!(w, "start_time_ticks  {}", m.proc.start_time_ticks)?;
    writeln!(
        w,
        "ctxt_switches     voluntary={} involuntary={}",
        m.proc.nr_voluntary_ctxt_switches, m.proc.nr_involuntary_ctxt_switches
    )?;
    writeln!(
        w,
        "io.chars          r={} w={}",
        m.proc.read_chars, m.proc.write_chars
    )?;
    writeln!(
        w,
        "io.bytes          r={} w={}",
        m.proc.read_bytes, m.proc.write_bytes
    )?;
    writeln!(
        w,
        "sched             run_ns={} wait_ns={} pcount={}",
        m.proc.sched_run_ns, m.proc.sched_wait_ns, m.proc.sched_pcount
    )?;
    if !m.cgroup.path.is_empty() {
        writeln!(w, "cgroup.path       {}", m.cgroup.path)?;
        if let Some(v) = m.cgroup.memory_current_bytes {
            writeln!(w, "cgroup.memory_current_bytes {v}")?;
        }
        if let Some(v) = m.cgroup.memory_peak_bytes {
            writeln!(w, "cgroup.memory_peak_bytes    {v}")?;
        }
        if let Some(v) = m.cgroup.memory_swap_current_bytes {
            writeln!(w, "cgroup.memory_swap_current  {v}")?;
        }
        if let Some(v) = m.cgroup.cpu_usage_usec {
            writeln!(w, "cgroup.cpu_usage_usec       {v}")?;
        }
        if let Some(v) = m.cgroup.cpu_user_usec {
            writeln!(w, "cgroup.cpu_user_usec        {v}")?;
        }
        if let Some(v) = m.cgroup.cpu_system_usec {
            writeln!(w, "cgroup.cpu_system_usec      {v}")?;
        }
        if let Some(v) = m.cgroup.pids_current {
            writeln!(w, "cgroup.pids_current         {v}")?;
        }
    }
    Ok(())
}

pub fn render_json<W: std::io::Write>(m: &Metrics, w: W) -> Result<()> {
    serde_json::to_writer_pretty(w, m).context("serialize metrics as json")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_status_extracts_vm_fields() {
        let s = "Name:\tuml\nState:\tR (running)\nThreads:\t4\nVmPeak:\t  204800 kB\n\
                 VmSize:\t  204800 kB\nVmRSS:\t  65536 kB\nVmSwap:\t      0 kB\n\
                 voluntary_ctxt_switches:\t123\nnonvoluntary_ctxt_switches:\t7\n";
        let mut p = ProcMetrics::default();
        parse_status(s, &mut p);
        assert_eq!(p.name, "uml");
        assert_eq!(p.state, "R (running)");
        assert_eq!(p.threads, 4);
        assert_eq!(p.vm_size_bytes, 204800 * 1024);
        assert_eq!(p.vm_rss_bytes, 65536 * 1024);
        assert_eq!(p.vm_peak_bytes, 204800 * 1024);
        assert_eq!(p.vm_swap_bytes, 0);
        assert_eq!(p.nr_voluntary_ctxt_switches, 123);
        assert_eq!(p.nr_involuntary_ctxt_switches, 7);
    }

    #[test]
    fn parse_stat_handles_comm_with_parens() {
        // `(linux (uml))` — comm with nested close-parens must
        // use rfind(')') to split.
        let s = "12345 (linux (uml)) R 2 3 4 5 6 7 8 9 10 11 \
                 1200 340 0 0 20 0 3 0 5000 999 777 \
                 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
        let mut p = ProcMetrics::default();
        parse_stat(s, &mut p);
        assert_eq!(p.utime_ticks, 1200);
        assert_eq!(p.stime_ticks, 340);
        assert_eq!(p.start_time_ticks, 5000);
    }

    #[test]
    fn parse_io_extracts_bytes_and_chars() {
        let s = "rchar: 100\nwchar: 200\nsyscr: 10\nsyscw: 20\n\
                 read_bytes: 4096\nwrite_bytes: 8192\ncancelled_write_bytes: 0\n";
        let mut p = ProcMetrics::default();
        parse_io(s, &mut p);
        assert_eq!(p.read_chars, 100);
        assert_eq!(p.write_chars, 200);
        assert_eq!(p.read_bytes, 4096);
        assert_eq!(p.write_bytes, 8192);
    }

    #[test]
    fn parse_schedstat_three_fields() {
        let s = "1234567 89012 345\n";
        let mut p = ProcMetrics::default();
        parse_schedstat(s, &mut p);
        assert_eq!(p.sched_run_ns, 1234567);
        assert_eq!(p.sched_wait_ns, 89012);
        assert_eq!(p.sched_pcount, 345);
    }

    #[test]
    fn cgroup_v2_path_parse() {
        let s = "12:devices:/user.slice\n\
                 0::/user.slice/user-1000.slice/session-42.scope\n\
                 5:memory:/user.slice\n";
        assert_eq!(
            parse_cgroup_v2_path(s),
            Some("/user.slice/user-1000.slice/session-42.scope".to_string())
        );
        assert_eq!(parse_cgroup_v2_path("12:devices:/foo"), None);
    }

    #[test]
    fn scrape_self_succeeds() {
        // Self-scrape — our own pid is always readable.
        let m = scrape(std::process::id()).expect("scrape self");
        assert_eq!(m.proc.pid, std::process::id());
        assert!(!m.proc.state.is_empty(), "state should be populated");
        assert!(m.proc.threads >= 1);
        assert!(m.proc.clock_hz >= 1);
    }

    #[test]
    fn scrape_dead_pid_errors() {
        // PID 0 never exists as a process.
        assert!(scrape(0).is_err());
    }

    #[test]
    fn render_human_emits_expected_fields() {
        let mut m = Metrics::default();
        m.proc.pid = 42;
        m.proc.name = "uml".into();
        m.proc.state = "S (sleeping)".into();
        m.proc.vm_rss_bytes = 8192;
        let mut buf = Vec::new();
        render_human(&m, &mut buf).unwrap();
        let s = String::from_utf8(buf).unwrap();
        assert!(s.contains("pid               42"));
        assert!(s.contains("name              uml"));
        assert!(s.contains("state             S (sleeping)"));
        assert!(s.contains("vm_rss_bytes      8192"));
    }

    #[test]
    fn render_json_is_valid_json() {
        let m = Metrics::default();
        let mut buf = Vec::new();
        render_json(&m, &mut buf).unwrap();
        let v: serde_json::Value = serde_json::from_slice(&buf).unwrap();
        assert!(v.get("proc").is_some());
        assert!(v.get("cgroup").is_some());
    }
}
