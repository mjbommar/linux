// SPDX-License-Identifier: GPL-2.0
//
// Per-run bundle metadata — the minimum slice of the
// observability-spine (memo 13) that Phase O1.1 lands.
//
// One `umlctl start` produces exactly one run; the run_id is
// a ULID (Crockford base32, lexicographic + time-sortable),
// and the run's bundle directory is `$STATE/runs/<run_id>/`.
// `run.json` at the root of that directory records the scalar
// metadata this file defines.
//
// Later spine phases add `events.jsonl`, `kernel.log`
// (separated from init.log), `trace.perfetto`, etc. alongside
// `run.json` in the same bundle directory.

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::io::Read;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use super::paths::Paths;

#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct Run {
    /// Schema version for the run.json payload itself. Frozen
    /// at 1 for the O1.1 lift; a future bump is additive-only
    /// until we need a migration.
    pub schema_version: u32,
    pub run_id: String,
    pub instance: String,
    /// SHA-256 of the kernel binary captured at create time;
    /// echoed here so a run.json is self-describing when the
    /// manifest is deleted before post-mortem.
    pub kernel_sha256: String,
    /// Wall clock (`CLOCK_REALTIME`) at the moment the parent
    /// finished spawning the child, RFC3339 with local offset.
    /// Used for human-readable audit; NOT used for correlation.
    pub created_at: String,
    /// `CLOCK_BOOTTIME` at spawn — monotonic since host boot,
    /// survives suspend/resume. This is the timestamp all
    /// future spine signals correlate against.
    pub host_ts_ns_at_exec: u64,
    pub pid: u32,
    /// Populated on stop. None means the run is still
    /// considered live (or the run was orphaned before a
    /// proper stop).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub exit_status: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub host_ts_ns_at_exit: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub signal_sent: Option<String>,
}

impl Run {
    pub fn write_to(&self, dir: &Path) -> Result<()> {
        let s = serde_json::to_string_pretty(self).context("serialize run.json")?;
        let path = dir.join("run.json");
        std::fs::write(&path, s).with_context(|| format!("write {}", path.display()))?;
        Ok(())
    }

    pub fn read(path: &Path) -> Result<Self> {
        let s =
            std::fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?;
        let r: Run = serde_json::from_str(&s)
            .with_context(|| format!("parse run.json {}", path.display()))?;
        Ok(r)
    }
}

/// `CLOCK_BOOTTIME` in nanoseconds — the clock the spine
/// correlates every signal against. Monotonic since host boot;
/// unlike CLOCK_MONOTONIC it advances while the system is
/// suspended, which matters for long-running UML instances.
pub fn boottime_ns() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let rc = unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut ts) };
    if rc != 0 {
        // Fallback: REALTIME. Shouldn't happen on Linux, but a
        // bad read is better than a panic at startup.
        let d = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default();
        return d.as_nanos() as u64;
    }
    (ts.tv_sec as u64) * 1_000_000_000 + (ts.tv_nsec as u64)
}

/// Generate a ULID as a 26-char Crockford-base32 string.
/// Format: 48-bit ms-since-epoch timestamp (10 chars) + 80-bit
/// random (16 chars). Lexicographic sort = chronological sort.
pub fn generate_run_id() -> String {
    let ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);

    let mut rand_bytes = [0u8; 10];
    // `/dev/urandom` is the unprivileged Linux source; ample
    // entropy after boot and there's no ceremony to import a
    // crate for something this small.
    if let Ok(mut f) = std::fs::File::open("/dev/urandom") {
        let _ = f.read_exact(&mut rand_bytes);
    }

    // Compose a 128-bit big-endian integer: [ts_ms (48b)] [rand (80b)].
    let mut bytes = [0u8; 16];
    bytes[0] = ((ms >> 40) & 0xff) as u8;
    bytes[1] = ((ms >> 32) & 0xff) as u8;
    bytes[2] = ((ms >> 24) & 0xff) as u8;
    bytes[3] = ((ms >> 16) & 0xff) as u8;
    bytes[4] = ((ms >> 8) & 0xff) as u8;
    bytes[5] = (ms & 0xff) as u8;
    bytes[6..16].copy_from_slice(&rand_bytes);

    crockford_base32_encode_128(&bytes)
}

/// Crockford base32 alphabet — no I/L/O/U, so run_ids don't
/// render ambiguously at the terminal. ULID spec encoding.
const CROCKFORD: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

fn crockford_base32_encode_128(bytes: &[u8; 16]) -> String {
    // ULID spec: treat the 128-bit value as a 130-bit number
    // with two zero bits prepended, then emit 26 × 5-bit
    // groups MSB-first. This is equivalent to shifting the
    // 128-bit reg left by 2 and reading 5 bits at a time.
    let mut reg: u128 = 0;
    for b in bytes.iter() {
        reg = (reg << 8) | (*b as u128);
    }
    // Reserve the top 2 bits for padding — always zero
    // because a valid 128-bit integer fits in 128 bits.
    let mut out = [0u8; 26];
    for i in 0..26 {
        // Group `i` (0 = MSB) occupies bits
        // [125-5i .. 121-5i] after the 2-bit pad. Compute
        // the shift: after placing the value at the top of
        // a 130-bit field, group i is at shift (125 - 5*i).
        let shift = 125 - 5 * (i as i32);
        let group = if shift >= 0 {
            ((reg >> shift) & 0x1f) as usize
        } else {
            ((reg << (-shift)) & 0x1f) as usize
        };
        out[i] = CROCKFORD[group];
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Create the run's bundle directory and write an initial
/// run.json — called from supervise::start after spawn.
pub fn create_run_bundle(
    paths: &Paths,
    run_id: &str,
    instance: &str,
    kernel_sha256: &str,
    host_ts_ns_at_exec: u64,
    pid: u32,
) -> Result<()> {
    let dir = paths.run_dir(run_id);
    std::fs::create_dir_all(&dir).with_context(|| format!("create run dir {}", dir.display()))?;
    let r = Run {
        schema_version: 1,
        run_id: run_id.to_string(),
        instance: instance.to_string(),
        kernel_sha256: kernel_sha256.to_string(),
        created_at: super::manifest::now_rfc3339(),
        host_ts_ns_at_exec,
        pid,
        exit_status: None,
        host_ts_ns_at_exit: None,
        signal_sent: None,
    };
    r.write_to(&dir)
}

/// Merge stop-time fields into the existing run.json. No-op if
/// the run.json can't be read — a broken bundle shouldn't block
/// the stop path.
pub fn finalize_run(
    paths: &Paths,
    run_id: &str,
    host_ts_ns_at_exit: u64,
    signal_sent: &str,
    exit_status: Option<i32>,
) {
    let dir = paths.run_dir(run_id);
    let path = dir.join("run.json");
    let Ok(mut r) = Run::read(&path) else { return };
    r.host_ts_ns_at_exit = Some(host_ts_ns_at_exit);
    r.signal_sent = Some(signal_sent.to_string());
    r.exit_status = exit_status;
    let _ = r.write_to(&dir);
}

/// Find the most recent run directory for `instance`. Used by
/// `logs` + future `dmesg` / `events` to resolve the default
/// bundle when no explicit run_id is named.
pub fn latest_run_for(paths: &Paths, instance: &str) -> Option<String> {
    let runs = paths.runs_dir();
    if !runs.exists() {
        return None;
    }
    let mut best: Option<(String, String)> = None; // (run_id, created_at)
    for entry in std::fs::read_dir(&runs).ok()? {
        let Ok(entry) = entry else { continue };
        if !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            continue;
        }
        let Some(name) = entry.file_name().to_str().map(String::from) else {
            continue;
        };
        let run_json = entry.path().join("run.json");
        let Ok(r) = Run::read(&run_json) else {
            continue;
        };
        if r.instance != instance {
            continue;
        }
        // ULID lexicographic order = chronological order; fine
        // to sort on run_id alone, but compare run_id strings
        // so a malformed run_json's created_at can't mislead.
        match &best {
            None => best = Some((name, r.run_id)),
            Some((_, cur_id)) if r.run_id > *cur_id => best = Some((name, r.run_id)),
            _ => {}
        }
    }
    best.map(|(name, _)| name)
}

/// Every run_id for an instance, chronologically newest-first.
pub fn runs_for(paths: &Paths, instance: &str) -> Vec<String> {
    let runs = paths.runs_dir();
    let Ok(entries) = std::fs::read_dir(&runs) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for entry in entries.flatten() {
        let Some(name) = entry.file_name().to_str().map(String::from) else {
            continue;
        };
        let Ok(r) = Run::read(&entry.path().join("run.json")) else {
            continue;
        };
        if r.instance == instance {
            out.push(name);
        }
    }
    out.sort();
    out.reverse();
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_id_shape() {
        let id = generate_run_id();
        assert_eq!(id.len(), 26, "ULID is 26 chars: {id}");
        assert!(id.chars().all(|c| CROCKFORD.contains(&(c as u8))));
    }

    #[test]
    fn run_ids_sort_chronologically() {
        // Two IDs generated 1 ms apart should compare correctly.
        let a = generate_run_id();
        std::thread::sleep(std::time::Duration::from_millis(2));
        let b = generate_run_id();
        assert!(b > a, "{b} should sort after {a}");
    }

    #[test]
    fn boottime_is_monotonic() {
        let a = boottime_ns();
        let b = boottime_ns();
        assert!(b >= a);
    }
}
