// SPDX-License-Identifier: GPL-2.0
//
// console_split — classify UML console output into "kernel
// printk" vs "everything else" and derive a `kernel.log`
// sidecar from the merged `init.log`.
//
// UML runs its init process on the same console fd that the
// kernel's printk subsystem writes to, so child stdout
// captures both streams. v1 splits them post-hoc at
// `umlctl stop` time (and on-the-fly for the `dmesg` verb
// while a run is live) by matching the two shapes that
// kernel printk lines wear:
//
//   - `[    0.123456]` — `printk_time` boot-timestamp prefix
//     (CONFIG_PRINTK_TIME=y, which UML has on by default).
//   - `<N>` — syslog-priority byte prefix used when the kernel
//     emits through /dev/kmsg-style paths.
//
// Anything that matches goes to kernel.log. Anything else
// (pre-printk UML init noise, userspace stdout, shell output,
// anything init prints) stays in init.log.
//
// This is a heuristic.  If a dedicated kernel-side event emitter is
// available, this module remains the fallback path for kernels that
// still multiplex printk and init output.

use std::io::{BufRead, BufReader, Write};
use std::path::Path;

/// True if `line` looks like a kernel printk line. Covers the
/// two prefixes UML's console emits:
///
///   - `[<seconds>.<microseconds>]` — PRINTK_TIME.
///   - `<N>` where N is a single ASCII digit — syslog priority.
///
/// Intentionally conservative: we'd rather misclassify a
/// kernel-ish userspace line into init.log than leak init
/// stdout into kernel.log.
pub fn is_kernel_line(line: &str) -> bool {
    if let Some(rest) = line.strip_prefix('[') {
        if let Some(end) = rest.find(']') {
            let inner = &rest[..end];
            // Drop leading whitespace that `printk_time` uses for
            // right-aligned seconds (`[    0.123456]`).
            let trimmed = inner.trim_start();
            if let Some((secs, usecs)) = trimmed.split_once('.') {
                let seconds_ok = !secs.is_empty() && secs.chars().all(|c| c.is_ascii_digit());
                let usecs_ok = !usecs.is_empty() && usecs.chars().all(|c| c.is_ascii_digit());
                if seconds_ok && usecs_ok {
                    return true;
                }
            }
        }
    }
    if let Some(rest) = line.strip_prefix('<') {
        if let Some(end) = rest.find('>') {
            let inner = &rest[..end];
            if !inner.is_empty() && inner.chars().all(|c| c.is_ascii_digit()) {
                return true;
            }
        }
    }
    false
}

/// Read every line of `init_log`, write the kernel-shaped ones
/// to `kernel_log`, and return how many lines were extracted.
/// Overwrites `kernel_log` if it already exists so re-running
/// the derive (e.g. an interrupted stop + resumed stop) yields
/// the same output.
///
/// Best-effort: a missing `init_log` is not an error — returns
/// `Ok(0)`. A write error on `kernel_log` is surfaced so the
/// caller can log it, but does not poison the stop path.
pub fn derive_kernel_log(init_log: &Path, kernel_log: &Path) -> std::io::Result<usize> {
    if !init_log.exists() {
        return Ok(0);
    }
    let f = std::fs::File::open(init_log)?;
    let reader = BufReader::new(f);
    let mut out = std::fs::File::create(kernel_log)?;
    let mut count = 0usize;
    for line in reader.lines() {
        let line = match line {
            Ok(s) => s,
            Err(_) => continue,
        };
        if is_kernel_line(&line) {
            out.write_all(line.as_bytes())?;
            out.write_all(b"\n")?;
            count += 1;
        }
    }
    Ok(count)
}

/// Stream the kernel-shaped lines of `init_log` to `out`
/// without materializing a sidecar file. Used by the
/// `umlctl dmesg` verb when the bundle doesn't yet have a
/// kernel.log (for example, a still-live run).
pub fn stream_kernel_lines<W: Write>(init_log: &Path, mut out: W) -> std::io::Result<usize> {
    if !init_log.exists() {
        return Ok(0);
    }
    let f = std::fs::File::open(init_log)?;
    let reader = BufReader::new(f);
    let mut count = 0usize;
    for line in reader.lines() {
        let line = match line {
            Ok(s) => s,
            Err(_) => continue,
        };
        if is_kernel_line(&line) {
            writeln!(out, "{line}")?;
            count += 1;
        }
    }
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_printk_timestamp() {
        assert!(is_kernel_line("[    0.000000] Linux version 7.0.0"));
        assert!(is_kernel_line(
            "[   12.345678] Booting Linux on physical CPU 0x0"
        ));
        assert!(is_kernel_line("[0.1] tight form"));
    }

    #[test]
    fn classifies_syslog_priority_prefix() {
        assert!(is_kernel_line("<4>random: crng init done"));
        assert!(is_kernel_line("<0>panic"));
        assert!(is_kernel_line("<7>debug"));
    }

    #[test]
    fn rejects_bare_init_stdout() {
        assert!(!is_kernel_line("Welcome to Linux"));
        assert!(!is_kernel_line("$ ls /etc"));
        assert!(!is_kernel_line(""));
        assert!(!is_kernel_line(
            "Checking that host ptys support output SIGIO..."
        ));
    }

    #[test]
    fn rejects_lookalikes() {
        // Bracket pair without a dotted float inside: treat as
        // userspace log formatting, not kernel printk.
        assert!(!is_kernel_line("[info] something"));
        assert!(!is_kernel_line("[1234] pid-style brackets"));
        assert!(!is_kernel_line("[.5] leading-dot malformed"));
        assert!(!is_kernel_line("[5.] trailing-dot malformed"));
        // `<N>` without a digit is not a priority byte.
        assert!(!is_kernel_line("<tag>html-looking"));
        assert!(!is_kernel_line("<>empty"));
    }

    #[test]
    fn derive_splits_mixed_log() {
        let tmp = tempdir();
        let init_log = tmp.join("init.log");
        let kernel_log = tmp.join("kernel.log");
        std::fs::write(
            &init_log,
            b"Checking that host ptys support output SIGIO...\n\
              [    0.000000] Linux version 7.0.0-uml\n\
              [    0.123456] Freeing unused kernel memory: 1024K\n\
              <4>random: crng init done\n\
              Welcome to Linux\n\
              $ echo hello\n\
              [   12.345678] KFENCE: initialized\n",
        )
        .unwrap();
        let n = derive_kernel_log(&init_log, &kernel_log).unwrap();
        assert_eq!(n, 4, "expected 4 kernel lines, got {n}");
        let out = std::fs::read_to_string(&kernel_log).unwrap();
        assert!(out.contains("Linux version 7.0.0-uml"));
        assert!(out.contains("Freeing unused kernel memory"));
        assert!(out.contains("crng init done"));
        assert!(out.contains("KFENCE: initialized"));
        assert!(!out.contains("Welcome to Linux"));
        assert!(!out.contains("$ echo hello"));
        std::fs::remove_dir_all(tmp).ok();
    }

    #[test]
    fn derive_missing_init_log_is_ok() {
        let tmp = tempdir();
        let init_log = tmp.join("init.log");
        let kernel_log = tmp.join("kernel.log");
        let n = derive_kernel_log(&init_log, &kernel_log).unwrap();
        assert_eq!(n, 0);
        assert!(!kernel_log.exists());
        std::fs::remove_dir_all(tmp).ok();
    }

    #[test]
    fn stream_emits_only_kernel_lines() {
        let tmp = tempdir();
        let init_log = tmp.join("init.log");
        std::fs::write(
            &init_log,
            b"userspace\n[    1.0] kernel-a\nmore user\n<3>kernel-b\n",
        )
        .unwrap();
        let mut buf = Vec::new();
        let n = stream_kernel_lines(&init_log, &mut buf).unwrap();
        assert_eq!(n, 2);
        let s = String::from_utf8(buf).unwrap();
        assert_eq!(s, "[    1.0] kernel-a\n<3>kernel-b\n");
        std::fs::remove_dir_all(tmp).ok();
    }

    fn tempdir() -> std::path::PathBuf {
        let base = std::env::temp_dir().join(format!(
            "umlctl-console-split-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&base).unwrap();
        base
    }
}
