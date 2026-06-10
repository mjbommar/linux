// SPDX-License-Identifier: GPL-2.0
//
// umlbuild shell — docker-style "drop into an interactive prompt inside
// the freshly-built instance."  Wraps `umlbuild instance` + a kernel
// launch with stdio wired to the user's terminal.
//
// Examples:
//
//   umlbuild shell                          # mvp profile, /bin/sh
//   umlbuild shell --profile sandbox
//   umlbuild shell --profile dev --cmd /bin/bash
//   umlbuild shell --profile mvp --cmd /usr/bin/python3   # Python REPL
//
// Unlike `umlctl up` (which supervises the kernel as a child and
// captures stdio to log files), `umlbuild shell` execve's into the
// kernel so the host TTY *is* the guest's console — Ctrl-C, line
// editing, REPL prompts all work.

use anyhow::{Context, Result};
use clap::Args;
use std::os::unix::io::AsRawFd;
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::{instance, profile};

#[derive(Args, Debug)]
pub struct ShellArgs {
    /// Profile name or path to a profile TOML.
    #[arg(long, value_name = "NAME_OR_PATH", default_value = "mvp")]
    pub profile: String,

    /// Instance directory.  Built (or rebuilt with --force) if missing.
    /// Defaults to a temp dir under $XDG_CACHE_HOME/uml-build/shells/.
    #[arg(long, value_name = "DIR")]
    pub out: Option<PathBuf>,

    /// Override the init command run inside the guest.  Default:
    /// "/bin/sh".  Common picks: "/bin/bash", "/usr/bin/python3",
    /// "/usr/bin/python3 -i" for REPL with -i, "/bin/ash".
    #[arg(long, value_name = "PATH", default_value = "/bin/sh")]
    pub cmd: String,

    /// Memory size override (defaults to the profile's instance.mem).
    #[arg(long, value_name = "SIZE")]
    pub mem: Option<String>,

    /// Force rebuild even if the instance dir already exists.
    #[arg(long)]
    pub force: bool,

    /// Source-tree path (forwarded to `umlbuild kernel`).
    #[arg(long, value_name = "PATH")]
    pub source: Option<PathBuf>,

    /// Networking mode.  "none" (default) gives the guest only
    /// loopback.  "tap" creates a host-side TAP device (via sudo),
    /// wires up MASQUERADE for outbound NAT, runs the kernel
    /// foregrounded, and tears the setup down on exit.  Requires the
    /// profile to define a [network] section + kernel Kconfigs for
    /// UML_NET_VECTOR.  Use the `sandbox-net` profile for a turnkey
    /// example.
    #[arg(long, value_name = "MODE", default_value = "none")]
    pub network: String,

    /// Outbound interface to NAT through ("auto" detects the host's
    /// default-route interface).  Only used when --network=tap.
    #[arg(long, value_name = "IFACE", default_value = "auto")]
    pub nat_via: String,

    /// Show the kernel boot banner and per-subsystem init messages.
    /// Default: pass `quiet` on the kernel cmdline so only warn-level
    /// and above reach the console — the REPL prompt is the first
    /// thing you see after the umlbuild-init line.
    #[arg(long)]
    pub verbose_kernel: bool,
}

pub fn run(args: ShellArgs) -> Result<()> {
    let prof = profile::resolve(&args.profile)?;

    // Default the instance out to a profile-keyed dir under XDG cache.
    let out = match args.out {
        Some(p) => p,
        None => {
            let cache = crate::paths::Paths::resolve()?;
            cache.cache_root.join("shells").join(&prof.profile.name)
        }
    };

    // Build / reuse the instance.
    let need_build =
        args.force || !out.join("linux").is_file() || !out.join("rootfs.img").is_file();
    if need_build {
        eprintln!(
            "umlbuild shell: building instance at {} (use --out to override)",
            out.display()
        );
        instance::run(instance::InstanceArgs {
            profile: args.profile.clone(),
            out: out.clone(),
            source: args.source.clone(),
            force: args.force,
        })?;
    } else {
        eprintln!(
            "umlbuild shell: reusing instance at {} (pass --force to rebuild)",
            out.display()
        );
    }

    let kernel = out.join("linux");
    let image = out.join("rootfs.img");
    if !kernel.is_file() || !image.is_file() {
        anyhow::bail!(
            "instance at {} is missing linux or rootfs.img after build",
            out.display()
        );
    }

    let mem = args.mem.unwrap_or_else(|| prof.instance.mem.clone());

    // Two ways to deliver the user's --cmd to the guest:
    //
    //   (a) `init=<cmd>` — fastest path, but bypasses /sbin/init which
    //       means the rootfs's network bring-up doesn't happen.  Fine
    //       for --network=none.
    //
    //   (b) `sandbox.cmdb64=<base64>` — the rootfs's /sbin/init runs
    //       (so guest-side IP / route / resolv.conf get set up from
    //       /etc/sandbox.net), then it exec's our cmd attached to the
    //       TTY.  Required for --network=tap.
    //
    // Path (b) is more uniform; we use it whenever the profile bakes
    // a network plan, otherwise (a) saves a couple of init lines.
    let use_init_dispatch = prof.network.mode == "tap" || args.network == "tap";

    let mut argv: Vec<String> = vec![
        format!("mem={mem}"),
        format!("ubd0={}", image.display()),
        "root=/dev/ubda".into(),
        "rw".into(),
        // fd:0 = stdin from launching terminal, fd:1 = stdout to it.
        // Format: con=IN,OUT (so con=fd:0,fd:1 means stdin from host
        // fd 0 + stdout to host fd 1).  All secondary consoles get
        // routed through null,fd:1 so they don't try to grab stdin.
        "con=fd:0,fd:1".into(),
        "con0=fd:0,fd:1".into(),
        "con1=null,fd:1".into(),
    ];

    // Suppress the kernel boot banner + per-subsystem init messages
    // unless --verbose-kernel is set.  `quiet` raises the console
    // loglevel; warnings and errors still come through.
    if !args.verbose_kernel {
        argv.push("quiet".into());
        argv.push("loglevel=4".into());
    }

    if use_init_dispatch {
        // Encode --cmd into the cmdline, let /sbin/init do its thing
        // (mount /proc, bring up vec0, set route, write resolv.conf,
        // then exec our cmd).
        let b64 = base64_encode(args.cmd.as_bytes());
        argv.push(format!("sandbox.cmdb64={b64}"));
    } else {
        // Direct override: /sbin/init never runs.
        let mut cmd_tokens = args
            .cmd
            .split_whitespace()
            .map(String::from)
            .collect::<Vec<_>>();
        let init_path = cmd_tokens
            .drain(..1)
            .next()
            .unwrap_or_else(|| "/bin/sh".to_string());
        argv.push(format!("init={init_path}"));
        argv.extend(cmd_tokens);
    }

    // Networking: when --network=tap, do sudo TAP setup, append the
    // vector= kernel token, run the kernel via fork+wait so we can
    // tear down after, then sudo-remove the TAP + iptables rules.
    let net_setup = if args.network == "tap" {
        if prof.network.mode != "tap" {
            anyhow::bail!(
                "--network=tap requires the profile to set [network].mode = \"tap\" \
                 (profile '{}' has mode = \"{}\"). Try --profile sandbox-net.",
                prof.profile.name,
                prof.network.mode
            );
        }
        let plan = NetworkSetup::plan(&prof.network, &args.nat_via)?;
        plan.bring_up()?;
        argv.push(plan.kernel_arg.clone());
        Some(plan)
    } else {
        None
    };

    let how = if use_init_dispatch {
        format!("via /sbin/init (sandbox.cmdb64=) cmd=\"{}\"", args.cmd)
    } else {
        format!("init={}", args.cmd)
    };
    eprintln!("umlbuild shell: launching {} ({how})", kernel.display());
    eprintln!("                Ctrl-D or `exit` to leave; kernel will power down.");
    eprintln!();

    // When our stdin is not a TTY (i.e. the user is piping or
    // redirecting), the UML console's `fd:0` source will see EOF
    // almost immediately and tear down the console before init can
    // produce any output.  Detect that and route the kernel's stdin
    // from /dev/null instead, which stays open for the lifetime of
    // the kernel.  For interactive use (host stdin IS a TTY), pass
    // through normally so line editing + Ctrl-C work.
    let stdin_is_tty = unsafe { libc::isatty(std::io::stdin().as_raw_fd()) } == 1;

    let result = if net_setup.is_some() {
        // fork+wait so we can run network teardown after the kernel exits.
        let mut cmd = Command::new(&kernel);
        cmd.args(&argv);
        if !stdin_is_tty {
            cmd.stdin(Stdio::null());
        }
        let status = cmd
            .status()
            .with_context(|| format!("spawn {}", kernel.display()));
        if let Some(plan) = &net_setup {
            plan.tear_down();
        }
        status.map(|_| ())
    } else if !stdin_is_tty {
        // Same batch-stdin rescue when there's no network setup —
        // but no teardown needed, so we can still use fork+wait.
        let mut cmd = Command::new(&kernel);
        cmd.args(&argv);
        cmd.stdin(Stdio::null());
        cmd.status()
            .with_context(|| format!("spawn {}", kernel.display()))
            .map(|_| ())
    } else {
        // Interactive + no network: execve to give the kernel the TTY directly.
        let err = Command::new(&kernel).args(&argv).exec();
        return Err(anyhow::Error::new(err).context(format!("execve {}", kernel.display())));
    };
    result
}

// ----------------------------------------------------------------------
// TAP-network bring-up / tear-down.
// ----------------------------------------------------------------------

/// Concrete plan for one shell invocation's networking.  Holds the
/// commands needed to bring the TAP up (sudo) and the symmetric
/// commands to tear it down on exit.
struct NetworkSetup {
    tap_name: String,
    /// host-side IP/prefix to assign to the TAP, e.g. "10.7.0.1/24"
    host_ip: String,
    /// network CIDR for MASQUERADE rule, e.g. "10.7.0.0/24"
    nat_cidr: String,
    nat_iface: String,
    kernel_arg: String,
}

impl NetworkSetup {
    fn plan(net: &profile::NetworkSpec, nat_via: &str) -> Result<Self> {
        let nat_cidr = cidr_of(&net.host_ip)?;
        let nat_iface = if nat_via == "auto" {
            detect_default_iface().unwrap_or_else(|| "eth0".to_string())
        } else {
            nat_via.to_string()
        };
        let kernel_arg = match net.driver.as_str() {
            "vector2" => format!(
                "vec2.0:transport=tap,mode=inproc,ifname={tap},depth=128",
                tap = net.tap_name
            ),
            _ => format!(
                "vec0:transport=tap,ifname={tap},depth=128",
                tap = net.tap_name
            ),
        };
        Ok(Self {
            tap_name: net.tap_name.clone(),
            host_ip: net.host_ip.clone(),
            nat_cidr,
            nat_iface,
            kernel_arg,
        })
    }

    /// Idempotent: if a previous run left the TAP behind, we delete
    /// it first.  All commands are routed through `sudo` (the user
    /// is expected to have NOPASSWD or be willing to type a password).
    fn bring_up(&self) -> Result<()> {
        let user = std::env::var("USER").unwrap_or_else(|_| "root".into());
        eprintln!(
            "umlbuild shell: setting up TAP {} (host_ip={}, nat_cidr={}, nat_via={})",
            self.tap_name, self.host_ip, self.nat_cidr, self.nat_iface
        );
        eprintln!("                (may prompt for sudo if not NOPASSWD)");

        // Idempotent cleanup of any leftover from a previous crash.
        // `ip link delete` is more reliable than `ip tuntap del` here:
        // the former works regardless of how the device was originally
        // opened, the latter requires the device to still be in a
        // matching tuntap state (and silently no-ops otherwise, leaving
        // the device behind for our subsequent `add` to collide with).
        let _ = sudo(&["ip", "link", "delete", &self.tap_name]);

        sudo_required(&[
            "ip",
            "tuntap",
            "add",
            "dev",
            &self.tap_name,
            "mode",
            "tap",
            "user",
            &user,
        ])?;
        sudo_required(&["ip", "addr", "add", &self.host_ip, "dev", &self.tap_name])?;
        sudo_required(&["ip", "link", "set", &self.tap_name, "up"])?;
        sudo_required(&["sysctl", "-w", "net.ipv4.ip_forward=1"])?;
        sudo_required(&[
            "iptables",
            "-t",
            "nat",
            "-A",
            "POSTROUTING",
            "-s",
            &self.nat_cidr,
            "-o",
            &self.nat_iface,
            "-j",
            "MASQUERADE",
        ])?;
        sudo_required(&[
            "iptables",
            "-A",
            "FORWARD",
            "-i",
            &self.tap_name,
            "-j",
            "ACCEPT",
        ])?;
        sudo_required(&[
            "iptables",
            "-A",
            "FORWARD",
            "-o",
            &self.tap_name,
            "-j",
            "ACCEPT",
        ])?;
        Ok(())
    }

    /// Best-effort: log but don't propagate failures — we want shell
    /// exit to be clean even if the user already removed the TAP
    /// out-of-band.
    fn tear_down(&self) {
        eprintln!("\numlbuild shell: tearing down TAP {}", self.tap_name);
        let _ = sudo(&[
            "iptables",
            "-D",
            "FORWARD",
            "-o",
            &self.tap_name,
            "-j",
            "ACCEPT",
        ]);
        let _ = sudo(&[
            "iptables",
            "-D",
            "FORWARD",
            "-i",
            &self.tap_name,
            "-j",
            "ACCEPT",
        ]);
        let _ = sudo(&[
            "iptables",
            "-t",
            "nat",
            "-D",
            "POSTROUTING",
            "-s",
            &self.nat_cidr,
            "-o",
            &self.nat_iface,
            "-j",
            "MASQUERADE",
        ]);
        let _ = sudo(&["ip", "link", "set", &self.tap_name, "down"]);
        let _ = sudo(&["ip", "tuntap", "del", "dev", &self.tap_name, "mode", "tap"]);
    }
}

/// `cidr_of("10.7.0.1/24")` → `"10.7.0.0/24"`.  Best-effort: returns
/// the input unchanged if it can't be parsed (which then triggers a
/// downstream iptables error the user will see).
fn cidr_of(ip_with_prefix: &str) -> Result<String> {
    let (ip, prefix) = ip_with_prefix
        .split_once('/')
        .ok_or_else(|| anyhow::anyhow!("expected IP/prefix, got {ip_with_prefix}"))?;
    let p: u8 = prefix.parse().context("parse prefix length")?;
    let octets: Vec<u8> = ip
        .split('.')
        .map(|s| s.parse::<u8>().context("parse IP octet"))
        .collect::<Result<_>>()?;
    if octets.len() != 4 {
        anyhow::bail!("expected dotted-quad IPv4, got {ip}");
    }
    let raw: u32 = ((octets[0] as u32) << 24)
        | ((octets[1] as u32) << 16)
        | ((octets[2] as u32) << 8)
        | (octets[3] as u32);
    let mask: u32 = if p == 0 { 0 } else { !0u32 << (32 - p) };
    let net = raw & mask;
    Ok(format!(
        "{}.{}.{}.{}/{p}",
        (net >> 24) & 0xff,
        (net >> 16) & 0xff,
        (net >> 8) & 0xff,
        net & 0xff,
    ))
}

/// Parse /proc/net/route to find the interface name behind the default
/// route.  Returns None if no default route is found or the file can't
/// be read.
fn detect_default_iface() -> Option<String> {
    let text = std::fs::read_to_string("/proc/net/route").ok()?;
    for line in text.lines().skip(1) {
        let cols: Vec<&str> = line.split_whitespace().collect();
        if cols.len() >= 3 && cols[1] == "00000000" {
            return Some(cols[0].to_string());
        }
    }
    None
}

/// Run `sudo <argv...>`, return the exit status.  Tolerant — returns
/// Ok even if the command failed; used for teardown where we want
/// best-effort.
fn sudo(argv: &[&str]) -> std::io::Result<std::process::ExitStatus> {
    Command::new("sudo").args(argv).status()
}

/// Like `sudo()` but propagates non-zero exits as errors.
fn sudo_required(argv: &[&str]) -> Result<()> {
    let status = sudo(argv).with_context(|| format!("sudo {}", argv.join(" ")))?;
    if !status.success() {
        anyhow::bail!("sudo {} failed: {status}", argv.join(" "));
    }
    Ok(())
}

/// Minimal RFC 4648 base64 encoder.  No padding-shenanigans, no
/// dep — keeps the launcher's transitive dep tree from growing.
fn base64_encode(input: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(input.len().div_ceil(3) * 4);
    for chunk in input.chunks(3) {
        let mut buf = [0u8; 3];
        for (i, b) in chunk.iter().enumerate() {
            buf[i] = *b;
        }
        let n = chunk.len();
        out.push(TABLE[(buf[0] >> 2) as usize] as char);
        out.push(TABLE[(((buf[0] & 0x03) << 4) | (buf[1] >> 4)) as usize] as char);
        out.push(if n > 1 {
            TABLE[(((buf[1] & 0x0f) << 2) | (buf[2] >> 6)) as usize] as char
        } else {
            '='
        });
        out.push(if n > 2 {
            TABLE[(buf[2] & 0x3f) as usize] as char
        } else {
            '='
        });
    }
    out
}
