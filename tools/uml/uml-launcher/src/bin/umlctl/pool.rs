// SPDX-License-Identifier: GPL-2.0
//
// umlctl pool — fork-server / template-pause integration (Memo 09).
//
// Phase 1b-MVP (this module) ships a single subcommand:
//
//   umlctl pool spawn --kernel PATH [--mem SIZE] [--cmdline EXTRA]
//                     [--instance NAME] [--mac MAC]
//                     [--tap NAME] [--ipv4 CIDR] [--gateway GW]
//                     [--json] [--foreground]
//
// `spawn` is the unit primitive of the template-pause + fork model:
//
//   1. Create a host memfd, write the requested identity blob.
//   2. Fork+exec the UML kernel with `um_template_pause` on the
//      cmdline and UM_TEMPLATE_IDENTITY_FD pointing at the memfd.
//   3. waitpid(WUNTRACED) — confirm the master SIGSTOPs itself at the
//      template_pause ready point.
//   4. Send SIGCONT — the master resumes, reads the identity blob
//      back from the memfd, applies it, and continues guest execution
//      as the named instance.
//   5. Print { pid, instance_name, mac, tap, ipv4_cidr } as JSON (or
//      a human-readable line) and return.
//
// Phase 1c (next session) adds a long-lived `umlctl pool serve`
// daemon that pre-spawns N members, exposes a Unix-socket take API,
// and pre-warms for syzkaller's create-on-slot-rotation cadence.
// Phase 1c's wire shape is forward-compatible with this MVP: a
// `pool take` against the daemon will return the same JSON envelope.
//
// What this DOES NOT do (yet):
//   - Apply the identity blob to in-kernel state (kernel-side Phase 2:
//     swap MAC, rebind IPv4, swap tap fd).  Today the kernel logs the
//     blob.  The identity *fields* round-trip end-to-end via memfd
//     here; the kernel can be wired to act on them in Phase 2.
//   - Manage > 1 member per master.  Each `pool spawn` boots a fresh
//     master, which becomes the taken instance.  N-fork-per-master
//     requires extending the kernel's um_template_pause_enter() to
//     loop on SIGSTOP/fork (Phase 2 in the kernel TU).
//   - Bundle-dir bookkeeping under $STATE/pools/<name>/.  Today the
//     command returns the pid; the caller is responsible for lifecycle.

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;
use std::ffi::CString;
use std::io::Write;
use std::os::fd::{AsRawFd, OwnedFd};
use std::os::unix::process::CommandExt;
use std::process::Command;

/// The on-wire identity blob.  Layout matches
/// `struct um_template_identity` in
/// arch/um/include/asm/um-template-pause.h: u32 magic, u32 version,
/// char[64] instance_name, u8[6] mac_addr, u8[2] _pad, char[16]
/// tap_name, char[20] ipv4_cidr, char[16] ipv4_gateway, char[96]
/// mconsole_path, u8[32] reserved.  Total 260 bytes.
const IDENTITY_BLOB_SIZE: usize = 4 + 4 + 64 + 6 + 2 + 16 + 20 + 16 + 96 + 32;
const IDENTITY_MAGIC: u32 = 0x44495455; // 'UTID' little-endian
const IDENTITY_VERSION: u32 = 1;

#[derive(Args, Debug)]
pub struct SpawnArgs {
    /// Path to the UML kernel binary.  Must be built with
    /// CONFIG_UM_TEMPLATE_PAUSE=y.
    #[arg(long, value_name = "PATH")]
    pub kernel: std::path::PathBuf,

    /// mem= argument passed on the cmdline.  Default 128M.
    #[arg(long, default_value = "128M", value_name = "SIZE")]
    pub mem: String,

    /// Extra cmdline appended after the template-pause-mandatory
    /// arguments.  Use this for `backend=`, `ncpus=`, `init=`, etc.
    #[arg(long, value_name = "STRING", default_value = "")]
    pub cmdline: String,

    /// Logical instance name written into the identity blob.
    #[arg(long, default_value = "pool-member-0", value_name = "NAME")]
    pub instance: String,

    /// MAC address as 6 colon-separated hex octets (e.g.
    /// 52:54:00:aa:bb:cc).  Written into the identity blob.
    #[arg(long, default_value = "52:54:00:aa:bb:cc", value_name = "MAC")]
    pub mac: String,

    /// New TAP device name.
    #[arg(long, default_value = "", value_name = "NAME")]
    pub tap: String,

    /// IPv4 address with CIDR (e.g. 10.7.0.42/24).
    #[arg(long, default_value = "", value_name = "CIDR")]
    pub ipv4: String,

    /// IPv4 gateway.
    #[arg(long, default_value = "", value_name = "ADDR")]
    pub gateway: String,

    /// Mconsole socket path (Phase 2+).
    #[arg(long, default_value = "", value_name = "PATH")]
    pub mconsole: String,

    /// Path to an init script invoked as the guest's PID 1.
    /// MUST eventually write to /proc/um/template_pause to trigger
    /// the pause — otherwise the kernel boots normally and `spawn`
    /// will time out waiting for SIGSTOP.
    ///
    /// If omitted, a default minimal script is written to a tempfile
    /// that mounts /proc, writes to /proc/um/template_pause, and
    /// then sleeps (so the host can observe the running instance).
    #[arg(long, value_name = "PATH")]
    pub init: Option<std::path::PathBuf>,

    /// rootfs args for the guest cmdline. Default = use hostfs at /.
    /// Override with e.g. "ubd0=/path/to/img" for a UBD root.
    #[arg(long, default_value = "rootfstype=hostfs rootflags=/ root=/dev/root rw",
          value_name = "STRING")]
    pub rootfs: String,

    /// Block waiting for the child to exit, streaming its stderr.
    #[arg(long)]
    pub foreground: bool,

    /// Print the resulting member as a JSON object on stdout.
    #[arg(long)]
    pub json: bool,
}

#[derive(serde::Serialize)]
pub struct SpawnResult {
    pub pid: i32,
    pub instance: String,
    pub mac: String,
    pub tap: String,
    pub ipv4_cidr: String,
    pub ipv4_gateway: String,
    pub mconsole_path: String,
    pub kernel: String,
    pub mem: String,
    pub identity_fd: i32,
    pub identity_blob_size: usize,
}

pub fn cmd_spawn(args: SpawnArgs, quiet: bool) -> Result<()> {
    if !args.kernel.exists() {
        bail!("kernel not found: {}", args.kernel.display());
    }
    let kernel_abs = std::fs::canonicalize(&args.kernel)
        .with_context(|| format!("canonicalize {}", args.kernel.display()))?;

    let mac_bytes = parse_mac(&args.mac)
        .with_context(|| format!("parse --mac {}", args.mac))?;
    let blob = build_identity_blob(
        &args.instance,
        &mac_bytes,
        &args.tap,
        &args.ipv4,
        &args.gateway,
        &args.mconsole,
    )?;
    assert_eq!(blob.len(), IDENTITY_BLOB_SIZE);

    let memfd = create_identity_memfd(&blob)
        .context("create identity memfd")?;
    let memfd_raw = memfd.as_raw_fd();

    // Resolve / synthesize an init script.  If user passed --init,
    // use it verbatim; else write a default to a tempfile.
    let init_path = match &args.init {
        Some(p) => p.clone(),
        None => default_init_script(&args.instance)
            .context("write default init script")?,
    };

    // Build argv.  All template-pause-mandatory args come first;
    // user-supplied --cmdline string is split on whitespace and
    // appended unchanged.
    let mut argv: Vec<String> = vec![
        kernel_abs.display().to_string(),
        format!("mem={}", args.mem),
        "um_template_pause".to_string(),
    ];
    for tok in args.rootfs.split_whitespace() {
        argv.push(tok.to_string());
    }
    argv.push(format!("init={}", init_path.display()));
    for tok in args.cmdline.split_whitespace() {
        argv.push(tok.to_string());
    }

    if !quiet {
        eprintln!(
            "umlctl pool spawn: kernel={} mem={} instance={} identity_fd={}",
            kernel_abs.display(), args.mem, args.instance, memfd_raw,
        );
        eprintln!("    cmdline: {}", argv[1..].join(" "));
    }

    // Spawn the UML.  pre_exec clears FD_CLOEXEC on the memfd so the
    // child inherits it; UM_TEMPLATE_IDENTITY_FD env var tells the
    // kernel where to find the blob.
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]);
    cmd.env("UM_TEMPLATE_IDENTITY_FD", memfd_raw.to_string());
    let memfd_for_preexec = memfd_raw;
    unsafe {
        cmd.pre_exec(move || {
            let flags = libc::fcntl(memfd_for_preexec, libc::F_GETFD);
            if flags < 0 {
                return Err(std::io::Error::last_os_error());
            }
            if libc::fcntl(
                memfd_for_preexec,
                libc::F_SETFD,
                flags & !libc::FD_CLOEXEC,
            ) < 0
            {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    if !args.foreground {
        cmd.stdout(std::process::Stdio::null());
        cmd.stderr(std::process::Stdio::null());
    }

    let child = cmd.spawn().with_context(|| {
        format!("spawn {} (is CONFIG_UM_TEMPLATE_PAUSE=y?)", argv[0])
    })?;
    let pid = child.id() as libc::pid_t;
    if !quiet {
        eprintln!("umlctl pool spawn: master pid={}, waiting for SIGSTOP…", pid);
    }

    // Wait for the master to SIGSTOP itself.  WUNTRACED makes
    // waitpid return on stop, not just exit.
    wait_for_sigstop(pid).with_context(|| {
        format!("wait for SIGSTOP from master pid={}", pid)
    })?;
    if !quiet {
        eprintln!("umlctl pool spawn: master SIGSTOPped; sending SIGCONT.");
    }

    // Hand over: SIGCONT and let the master resume with the identity
    // applied.  Do NOT waitpid further unless foreground was asked.
    let r = unsafe { libc::kill(pid, libc::SIGCONT) };
    if r < 0 {
        return Err(std::io::Error::last_os_error())
            .context("SIGCONT master")?;
    }

    let result = SpawnResult {
        pid,
        instance: args.instance.clone(),
        mac: args.mac.clone(),
        tap: args.tap.clone(),
        ipv4_cidr: args.ipv4.clone(),
        ipv4_gateway: args.gateway.clone(),
        mconsole_path: args.mconsole.clone(),
        kernel: kernel_abs.display().to_string(),
        mem: args.mem.clone(),
        identity_fd: memfd_raw,
        identity_blob_size: blob.len(),
    };

    if args.json {
        let s = serde_json::to_string(&result)
            .context("serialize spawn result")?;
        println!("{}", s);
    } else if !quiet {
        println!(
            "pool member spawned: pid={} instance={} mac={} ipv4={}",
            result.pid, result.instance, result.mac, result.ipv4_cidr,
        );
    }

    if args.foreground {
        // Re-attach: keep the memfd alive, wait for the child to
        // exit.  Drop the memfd after the wait so the child can
        // still re-read it post-SIGCONT if it wants.
        let r = unsafe { libc::waitpid(pid, std::ptr::null_mut(), 0) };
        if r < 0 {
            return Err(std::io::Error::last_os_error())
                .context("foreground waitpid")?;
        }
    } else {
        // Background mode: drop ownership of the memfd by leaking
        // the OwnedFd; the kernel keeps the file open via the
        // inherited child fd, so the memfd's underlying inode
        // outlives our process.  (The child can later re-read or
        // ignore the blob; either way our exit doesn't tear it down.)
        std::mem::forget(memfd);
    }

    Ok(())
}

/// Write a minimal init script to a tempfile and return its path.
/// The script mounts /proc, writes a tag to /proc/um/template_pause
/// (which blocks until SIGCONT arrives), then sleeps to keep the
/// resumed instance alive long enough for `pool spawn`'s caller to
/// observe / interact with it.  Override with --init for anything
/// non-trivial.
fn default_init_script(instance: &str) -> Result<std::path::PathBuf> {
    let dir = std::env::temp_dir();
    let path = dir.join(format!("umlctl-pool-init-{}.sh", std::process::id()));
    let content = format!(
        "#!/bin/sh\n\
         # auto-generated by umlctl pool spawn for instance={instance}\n\
         mount -t proc proc /proc 2>/dev/null\n\
         echo \"POOL_MEMBER_BOOTING\"\n\
         echo \"{instance}\" > /proc/um/template_pause\n\
         echo \"POOL_MEMBER_RESUMED\"\n\
         # Keep the instance alive; Phase 1c will replace this with\n\
         # the user-payload exec or a syzkaller-style command runner.\n\
         exec sleep infinity\n",
        instance = instance,
    );
    std::fs::write(&path, content)
        .with_context(|| format!("write {}", path.display()))?;
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755))
        .with_context(|| format!("chmod {}", path.display()))?;
    Ok(path)
}

fn parse_mac(s: &str) -> Result<[u8; 6]> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != 6 {
        bail!("expected 6 colon-separated octets, got {}", parts.len());
    }
    let mut out = [0u8; 6];
    for (i, p) in parts.iter().enumerate() {
        out[i] = u8::from_str_radix(p, 16)
            .with_context(|| format!("octet {} not hex: {:?}", i, p))?;
    }
    Ok(out)
}

fn build_identity_blob(
    instance: &str,
    mac: &[u8; 6],
    tap: &str,
    ipv4: &str,
    gateway: &str,
    mconsole: &str,
) -> Result<Vec<u8>> {
    fn pad(s: &str, n: usize) -> Result<Vec<u8>> {
        let mut v = s.as_bytes().to_vec();
        if v.len() >= n {
            bail!("field too long: {} bytes (limit {})", v.len(), n - 1);
        }
        v.resize(n, 0);
        Ok(v)
    }
    let mut blob = Vec::with_capacity(IDENTITY_BLOB_SIZE);
    blob.extend_from_slice(&IDENTITY_MAGIC.to_le_bytes());
    blob.extend_from_slice(&IDENTITY_VERSION.to_le_bytes());
    blob.extend_from_slice(&pad(instance, 64)?);
    blob.extend_from_slice(mac);
    blob.extend_from_slice(&[0u8, 0u8]); // _pad
    blob.extend_from_slice(&pad(tap, 16)?);
    blob.extend_from_slice(&pad(ipv4, 20)?);
    blob.extend_from_slice(&pad(gateway, 16)?);
    blob.extend_from_slice(&pad(mconsole, 96)?);
    blob.extend_from_slice(&[0u8; 32]); // reserved
    Ok(blob)
}

fn create_identity_memfd(blob: &[u8]) -> Result<OwnedFd> {
    let name = CString::new("um-pool-identity").unwrap();
    // MFD_CLOEXEC = 1; we explicitly clear cloexec in pre_exec
    // before execve so the child inherits the fd.
    let fd = unsafe { libc::memfd_create(name.as_ptr(), libc::MFD_CLOEXEC) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error())
            .context("memfd_create");
    }
    let owned: OwnedFd = unsafe { std::os::fd::FromRawFd::from_raw_fd(fd) };
    let mut f = std::fs::File::from(owned.try_clone().context("dup memfd")?);
    f.write_all(blob).context("write identity blob")?;
    use std::io::Seek;
    f.seek(std::io::SeekFrom::Start(0))
        .context("rewind identity memfd")?;
    drop(f);
    Ok(owned)
}

/// Block until the named child reports a stop (WUNTRACED).  Returns
/// Ok(()) on stop, an error on early exit or syscall failure.
fn wait_for_sigstop(pid: libc::pid_t) -> Result<()> {
    use std::time::{Duration, Instant};
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let mut status: libc::c_int = 0;
        let r = unsafe {
            libc::waitpid(pid, &mut status as *mut _, libc::WUNTRACED | libc::WNOHANG)
        };
        if r < 0 {
            return Err(std::io::Error::last_os_error())
                .context("waitpid WUNTRACED");
        }
        if r == 0 {
            if Instant::now() > deadline {
                return Err(anyhow!(
                    "timeout waiting for master pid {} to SIGSTOP",
                    pid
                ));
            }
            std::thread::sleep(Duration::from_millis(50));
            continue;
        }
        // r == pid here.
        if libc::WIFSTOPPED(status) {
            let sig = libc::WSTOPSIG(status);
            if sig != libc::SIGSTOP {
                return Err(anyhow!(
                    "master stopped with unexpected signal {} (expected SIGSTOP={})",
                    sig, libc::SIGSTOP
                ));
            }
            return Ok(());
        }
        if libc::WIFEXITED(status) {
            return Err(anyhow!(
                "master exited (code {}) before SIGSTOP",
                libc::WEXITSTATUS(status)
            ));
        }
        if libc::WIFSIGNALED(status) {
            return Err(anyhow!(
                "master killed by signal {} before SIGSTOP",
                libc::WTERMSIG(status)
            ));
        }
        // Continue spinning.
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mac_roundtrip() {
        let m = parse_mac("52:54:00:aa:bb:cc").unwrap();
        assert_eq!(m, [0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc]);
        assert!(parse_mac("52:54:00:aa:bb").is_err());
        assert!(parse_mac("52:54:00:aa:bb:gg").is_err());
    }

    #[test]
    fn identity_blob_layout() {
        let mac = [0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc];
        let blob = build_identity_blob(
            "pool-member-0",
            &mac,
            "tap-pool0",
            "10.7.0.42/24",
            "10.7.0.1",
            "/tmp/mconsole-0.sock",
        )
        .unwrap();
        assert_eq!(blob.len(), IDENTITY_BLOB_SIZE);
        // Magic + version at offset 0.
        assert_eq!(
            u32::from_le_bytes(blob[0..4].try_into().unwrap()),
            IDENTITY_MAGIC
        );
        assert_eq!(
            u32::from_le_bytes(blob[4..8].try_into().unwrap()),
            IDENTITY_VERSION
        );
        // Instance name starts at offset 8.
        let name_end = blob[8..72].iter().position(|&b| b == 0).unwrap_or(64);
        assert_eq!(&blob[8..8 + name_end], b"pool-member-0");
        // MAC at offset 72.
        assert_eq!(&blob[72..78], &mac);
    }

    #[test]
    fn identity_blob_overlong_field_rejected() {
        let mac = [0u8; 6];
        let too_long_name = "x".repeat(64);
        let r = build_identity_blob(
            &too_long_name,
            &mac,
            "",
            "",
            "",
            "",
        );
        assert!(r.is_err());
    }
}
