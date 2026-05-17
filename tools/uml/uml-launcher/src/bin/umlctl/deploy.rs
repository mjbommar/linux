// SPDX-License-Identifier: GPL-2.0
//
// umlctl `up`/`down` — declarative UML deployment from a single
// TOML "Umlfile". Inspired by docker-compose.yml: one file
// describes kernel, network, ports, mounts, env, init phases.
//
// The promise: `umlctl up -f Umlfile.toml` brings a working
// service up end-to-end (TAP+NAT+port-forward+resolv.conf+
// in-guest networking+the actual workload) without the user
// hand-rolling iptables and init scripts. `umlctl down` tears
// it down cleanly.
//
// Why this exists: see Documentation/virt/uml/redesign/
// 08-future-phases/05-umlctl.md (umlctl spec) and the
// post-mortem of the 2026-04-29 fastapi-on-UML session that
// motivated this — every path-name and port-forward had to be
// re-derived because the lifecycle CLI didn't know about
// network or workloads, just kernels.
//
// Keep this file additive — the existing `create`/`start`/
// `stop` verbs stay as the low-level primitives; `up`/`down`
// are sugar that compiles an Umlfile down to a manifest +
// host-side setup steps + a generated init script.

use anyhow::{anyhow, bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::manifest;

/// Schema version of the Umlfile format. Bumped when wire-format
/// breaks. Today only v1.
const UMLFILE_SCHEMA_VERSION: u32 = 1;

/// Top-level Umlfile. Fields with a `default` annotation are
/// optional; the rest must be set or a parse error fires at
/// `from_path`.
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct Umlfile {
    pub schema_version: u32,
    pub instance: InstanceSection,
    pub kernel: KernelSection,
    #[serde(default)]
    pub runtime: RuntimeSection,
    #[serde(default)]
    pub network: NetworkSection,
    #[serde(default)]
    pub volumes: Vec<VolumeSection>,
    #[serde(default)]
    pub env: BTreeMap<String, String>,
    #[serde(default)]
    pub init: InitSection,
    #[serde(default)]
    pub debug: DebugSection,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct InstanceSection {
    pub name: String,
    #[serde(default)]
    pub labels: BTreeMap<String, String>,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct KernelSection {
    pub path: String,
    #[serde(default = "default_backend")]
    pub backend: String,
    #[serde(default)]
    pub append: Vec<String>,
}

fn default_backend() -> String {
    "seccomp".into()
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields, default)]
pub struct RuntimeSection {
    pub mem: String,
    pub ncpus: u32,
}

impl Default for RuntimeSection {
    fn default() -> Self {
        Self {
            mem: "512M".into(),
            ncpus: 1,
        }
    }
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields, default)]
pub struct NetworkSection {
    /// "none" (default) or "tap".
    pub mode: String,
    /// "vector" (legacy vec0) or "vector2" (experimental vec2.0).
    pub driver: String,
    pub tap_name: String,
    pub guest_ip: String,
    pub host_ip: String,
    pub gateway: String,
    pub nameservers: Vec<String>,
    /// "auto" picks the host's default-route iface; or e.g. "enp3s0".
    pub masquerade_via: String,
    /// Each entry: "host_port:guest_port[/proto]" — proto defaults
    /// to tcp. Implemented via host-side DNAT to guest_ip:guest_port.
    pub ports: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct NetworkPlan {
    pub driver: String,
    pub guest_dev: String,
    pub tap_name: String,
    pub transport: String,
    pub host_mode: String,
    pub queue_count: u32,
    pub kernel_arg: String,
}

impl Default for NetworkSection {
    fn default() -> Self {
        Self {
            mode: "none".into(),
            driver: "vector".into(),
            tap_name: "uml-tap0".into(),
            guest_ip: "10.7.0.2/24".into(),
            host_ip: "10.7.0.1/24".into(),
            gateway: "10.7.0.1".into(),
            nameservers: vec!["8.8.8.8".into(), "1.1.1.1".into()],
            masquerade_via: "auto".into(),
            ports: Vec::new(),
        }
    }
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct VolumeSection {
    pub src: String,
    pub dst: String,
    /// "ro" (default) | "rw"
    #[serde(default = "default_volume_mode")]
    pub mode: String,
}

fn default_volume_mode() -> String {
    "ro".into()
}

#[derive(Serialize, Deserialize, Debug, Clone, Default)]
#[serde(deny_unknown_fields, default)]
pub struct InitSection {
    /// Sequence of named phases run after kernel boot + networking.
    /// Each is a single shell command; failures abort.
    pub phases: Vec<InitPhase>,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct InitPhase {
    pub name: String,
    pub cmd: String,
    /// If set, the phase is considered "ready" (and we move to the
    /// next phase) when stdout/stderr matches this substring. If
    /// the cmd exits before the marker prints, the phase fails.
    /// If empty, we just wait for the cmd to exit 0.
    #[serde(default)]
    pub expect: String,
    /// Per-phase timeout in seconds. 0 = inherit.
    #[serde(default)]
    pub timeout_secs: u32,
}

#[derive(Serialize, Deserialize, Debug, Clone, Default)]
#[serde(deny_unknown_fields, default)]
pub struct DebugSection {
    /// Wrap UML in `strace -f -s 256 -o <log_dir>/strace.log`.
    pub strace: bool,
    /// Run UML under `gdbserver :<port>` so a remote gdb can attach.
    pub gdb: bool,
    /// gdbserver listen port.
    pub gdb_port: u16,
    /// Per-deployment log directory (relative to cwd or absolute).
    /// Defaults to "./logs/<instance-name>/".
    pub log_dir: String,
    /// Keep the UML process alive (and host-side TAP/iptables) when
    /// an init phase fails — useful for poking around with `umlctl
    /// exec` / `umlctl logs`.
    pub keep_running_on_failure: bool,
}

// -------------------------------------------------------------------
// Loading + validation
// -------------------------------------------------------------------

impl Umlfile {
    /// Read + parse + validate. Expand `$HOME` and `$VAR` in path-
    /// like fields (kernel.path, volume.src/dst). Future: support
    /// `~/...` expansion explicitly via shellexpand.
    pub fn from_path(path: &Path) -> Result<Self> {
        let s =
            fs::read_to_string(path).with_context(|| format!("read Umlfile {}", path.display()))?;
        let mut u: Umlfile =
            toml::from_str(&s).with_context(|| format!("parse Umlfile {}", path.display()))?;

        if u.schema_version != UMLFILE_SCHEMA_VERSION {
            bail!(
                "Umlfile {} has schema_version {} (this umlctl handles v{})",
                path.display(),
                u.schema_version,
                UMLFILE_SCHEMA_VERSION,
            );
        }

        manifest::validate_name(&u.instance.name)?;
        u.kernel.path = expand_env(&u.kernel.path);
        for env_v in u.env.values_mut() {
            *env_v = expand_env(env_v);
        }
        for phase in &mut u.init.phases {
            phase.cmd = expand_env(&phase.cmd);
        }
        for v in &mut u.volumes {
            v.src = expand_env(&v.src);
            v.dst = expand_env(&v.dst);
            if v.mode != "ro" && v.mode != "rw" {
                bail!(
                    "volume {} → {}: mode must be 'ro' or 'rw' (got {:?})",
                    v.src,
                    v.dst,
                    v.mode
                );
            }
        }
        validate_network_section(&u.network)?;
        for p in &u.network.ports {
            parse_port_forward(p).with_context(|| format!("parse network.ports[{}]", p))?;
        }
        if u.debug.gdb_port == 0 {
            u.debug.gdb_port = 5678;
        }
        if u.debug.log_dir.is_empty() {
            u.debug.log_dir = format!("logs/{}", u.instance.name);
        }
        Ok(u)
    }
}

pub fn validate_network_driver(driver: &str) -> Result<()> {
    if !["vector", "vector2"].contains(&driver) {
        bail!(
            "network.driver must be 'vector' or 'vector2' (got {:?})",
            driver,
        );
    }
    Ok(())
}

pub fn set_network_driver(uml: &mut Umlfile, driver: &str) -> Result<()> {
    validate_network_driver(driver)?;
    if uml.network.mode != "tap" && driver != "vector" {
        bail!("network.driver is only meaningful when network.mode = 'tap'");
    }
    uml.network.driver = driver.to_string();
    Ok(())
}

fn validate_network_section(net: &NetworkSection) -> Result<()> {
    if !["none", "tap"].contains(&net.mode.as_str()) {
        bail!("network.mode must be 'none' or 'tap' (got {:?})", net.mode);
    }
    validate_network_driver(&net.driver)?;
    if net.mode != "tap" && net.driver != "vector" {
        bail!("network.driver is only meaningful when network.mode = 'tap'");
    }
    Ok(())
}

/// Expand $VAR / ${VAR} / $HOME from process env. Unknown vars
/// expand to empty (not an error — gives us "kernel.path = $UML_KERNEL"
/// usability without per-Umlfile gating).
fn expand_env(s: &str) -> String {
    shellexpand::env(s)
        .map(|c| c.to_string())
        .unwrap_or_else(|_| s.to_string())
}

/// Parse "8765:8765/tcp" → (host=8765, guest=8765, proto="tcp").
/// Proto defaults to tcp when omitted.
pub fn parse_port_forward(s: &str) -> Result<(u16, u16, String)> {
    let (mapping, proto) = match s.split_once('/') {
        Some((m, p)) => (m, p.to_string()),
        None => (s, "tcp".into()),
    };
    if !["tcp", "udp"].contains(&proto.as_str()) {
        bail!("port forward proto must be tcp|udp (got {:?})", proto);
    }
    let (h, g) = mapping
        .split_once(':')
        .ok_or_else(|| anyhow!("port forward must be HOST:GUEST[/proto], got {:?}", s))?;
    let host: u16 = h
        .parse()
        .with_context(|| format!("port forward host port {:?}", h))?;
    let guest: u16 = g
        .parse()
        .with_context(|| format!("port forward guest port {:?}", g))?;
    Ok((host, guest, proto))
}

fn tap_network_plan(net: &NetworkSection) -> NetworkPlan {
    match net.driver.as_str() {
        "vector2" => NetworkPlan {
            driver: "vector2".into(),
            guest_dev: "vec2.0".into(),
            tap_name: net.tap_name.clone(),
            transport: "tap".into(),
            host_mode: "inproc".into(),
            queue_count: 1,
            kernel_arg: format!(
                "vec2.0:transport=tap,mode=inproc,ifname={tap},depth=128",
                tap = net.tap_name,
            ),
        },
        _ => NetworkPlan {
            driver: "vector".into(),
            guest_dev: "vec0".into(),
            tap_name: net.tap_name.clone(),
            transport: "tap".into(),
            host_mode: "legacy-inproc".into(),
            queue_count: 1,
            kernel_arg: format!(
                "vec0:transport=tap,ifname={tap},depth=128",
                tap = net.tap_name,
            ),
        },
    }
}

// -------------------------------------------------------------------
// Compile: Umlfile → (host setup actions, init script, kernel cmdline)
// -------------------------------------------------------------------

/// What we produced by compiling an Umlfile. The caller (cmd_up)
/// executes these in order: setup, then create+start the manifest,
/// then later teardown on `down`.
#[derive(Debug)]
pub struct Compiled {
    /// Generated init script absolute path (under log_dir).
    pub init_script: PathBuf,
    /// Append entries to add to the kernel cmdline (joined by space).
    pub append: Vec<String>,
    /// Host-side setup steps (TAP, iptables, etc) to run as sudo.
    /// Each entry is a `sh -c`-able command.
    pub setup_steps: Vec<String>,
    /// Host-side teardown steps (the inverses of setup_steps), best-effort.
    pub teardown_steps: Vec<String>,
    /// Effective TAP network plan, when `[network].mode = "tap"`.
    pub network_plan: Option<NetworkPlan>,
}

pub fn compile(uml: &Umlfile) -> Result<Compiled> {
    let log_dir = std::env::current_dir()
        .context("getcwd")?
        .join(&uml.debug.log_dir);
    fs::create_dir_all(&log_dir).with_context(|| format!("mkdir -p {}", log_dir.display()))?;

    let mut append = uml.kernel.append.clone();
    append.push(format!("backend=force={}", uml.kernel.backend));

    let mut setup_steps = Vec::new();
    let mut teardown_steps = Vec::new();
    let mut network_plan = None;

    if uml.network.mode == "tap" {
        let net = &uml.network;
        let plan = tap_network_plan(net);
        let masq_iface = if net.masquerade_via == "auto" {
            detect_default_iface().unwrap_or_else(|_| "eth0".into())
        } else {
            net.masquerade_via.clone()
        };

        // Setup: ip tuntap add → addr → up → forwarding → MASQUERADE → port-forwards
        let user = std::env::var("USER").unwrap_or_else(|_| "uml".into());
        setup_steps.push(format!(
            "ip tuntap add dev {tap} mode tap user {user}",
            tap = net.tap_name,
            user = user,
        ));
        setup_steps.push(format!(
            "ip addr add {host_ip} dev {tap}",
            host_ip = net.host_ip,
            tap = net.tap_name,
        ));
        setup_steps.push(format!("ip link set {tap} up", tap = net.tap_name));
        setup_steps.push("sysctl -w net.ipv4.ip_forward=1".into());
        let cidr = guest_cidr(&net.guest_ip)?;
        setup_steps.push(format!(
            "iptables -t nat -A POSTROUTING -s {cidr} -o {iface} -j MASQUERADE",
            cidr = cidr,
            iface = masq_iface,
        ));
        setup_steps.push(format!(
            "iptables -A FORWARD -i {tap} -j ACCEPT",
            tap = net.tap_name,
        ));
        setup_steps.push(format!(
            "iptables -A FORWARD -o {tap} -j ACCEPT",
            tap = net.tap_name,
        ));

        let guest_ip_only = guest_ip_addr(&net.guest_ip)?;
        // PREROUTING catches packets entering from external interfaces;
        // OUTPUT catches host-local traffic (e.g. `curl 127.0.0.1:HOST_PORT`)
        // since locally-generated packets bypass PREROUTING. We need both
        // for the `host:guest` mapping to feel docker-like.
        // route_localnet=1 lets the kernel route 127.0.0.0/8 destinations
        // through the rewritten next-hop instead of dropping them as martians.
        setup_steps.push(format!(
            "sysctl -w net.ipv4.conf.{tap}.route_localnet=1",
            tap = net.tap_name,
        ));
        for p in &net.ports {
            let (host_port, guest_port, proto) = parse_port_forward(p)?;
            setup_steps.push(format!(
                "iptables -t nat -A PREROUTING -p {proto} --dport {host_port} -j DNAT --to-destination {guest}:{guest_port}",
                proto = proto, host_port = host_port,
                guest = guest_ip_only, guest_port = guest_port,
            ));
            setup_steps.push(format!(
                "iptables -t nat -A OUTPUT -p {proto} -d 127.0.0.0/8 --dport {host_port} -j DNAT --to-destination {guest}:{guest_port}",
                proto = proto, host_port = host_port,
                guest = guest_ip_only, guest_port = guest_port,
            ));
            // SNAT host-local replies back through 10.7.0.1 so the guest's
            // reply path (uml-tap0 → host) is symmetric.
            setup_steps.push(format!(
                "iptables -t nat -A POSTROUTING -p {proto} -d {guest} --dport {guest_port} -j SNAT --to-source {host_ip_only}",
                proto = proto, guest = guest_ip_only, guest_port = guest_port,
                host_ip_only = guest_ip_addr(&net.host_ip)?,
            ));
        }

        // Teardown: undo in reverse. Each `iptables -D` is best-effort.
        for p in net.ports.iter().rev() {
            if let Ok((host_port, guest_port, proto)) = parse_port_forward(p) {
                teardown_steps.push(format!(
                    "iptables -t nat -D POSTROUTING -p {proto} -d {guest} --dport {guest_port} -j SNAT --to-source {host_ip_only}",
                    proto = proto, guest = guest_ip_only, guest_port = guest_port,
                    host_ip_only = guest_ip_addr(&net.host_ip)?,
                ));
                teardown_steps.push(format!(
                    "iptables -t nat -D OUTPUT -p {proto} -d 127.0.0.0/8 --dport {host_port} -j DNAT --to-destination {guest}:{guest_port}",
                    proto = proto, host_port = host_port,
                    guest = guest_ip_only, guest_port = guest_port,
                ));
                teardown_steps.push(format!(
                    "iptables -t nat -D PREROUTING -p {proto} --dport {host_port} -j DNAT --to-destination {guest}:{guest_port}",
                    proto = proto, host_port = host_port,
                    guest = guest_ip_only, guest_port = guest_port,
                ));
            }
        }
        teardown_steps.push(format!(
            "iptables -D FORWARD -o {tap} -j ACCEPT",
            tap = net.tap_name,
        ));
        teardown_steps.push(format!(
            "iptables -D FORWARD -i {tap} -j ACCEPT",
            tap = net.tap_name,
        ));
        teardown_steps.push(format!(
            "iptables -t nat -D POSTROUTING -s {cidr} -o {iface} -j MASQUERADE",
            cidr = cidr,
            iface = masq_iface,
        ));
        teardown_steps.push(format!("ip link set {tap} down", tap = net.tap_name,));
        teardown_steps.push(format!(
            "ip tuntap del dev {tap} mode tap",
            tap = net.tap_name,
        ));

        // Vector cmdline arg (one big quoted token; the kernel sees it
        // because UML's __setup parser walks cmdline tokens).
        append.push(plan.kernel_arg.clone());
        network_plan = Some(plan);
    }

    // Generate init script.
    let init_script = log_dir.join("init.sh");
    let init_text = render_init_script(uml)?;
    fs::write(&init_script, init_text)
        .with_context(|| format!("write {}", init_script.display()))?;
    let mut perm = fs::metadata(&init_script)?.permissions();
    perm.set_mode(0o755);
    fs::set_permissions(&init_script, perm)?;

    Ok(Compiled {
        init_script,
        append,
        setup_steps,
        teardown_steps,
        network_plan,
    })
}

/// Render the in-guest init script. Stdlib bash, no fancy deps.
/// Sequence: tmpfs /etc → resolv.conf → selected netdev up →
/// volume bind-mounts → env exports → init phases.
fn render_init_script(uml: &Umlfile) -> Result<String> {
    let mut s = String::new();
    s.push_str("#!/bin/bash\n");
    s.push_str("# Auto-generated by `umlctl up`. Do not edit by hand —\n");
    s.push_str("# regenerated on every `up` from the Umlfile.\n");
    s.push_str("set +e\n");
    s.push_str("\n");

    // Default environment. The Linux kernel passes a near-empty env
    // to init (no PATH, no HOME, no TERM by default). Without these,
    // tools that resolve themselves via argv[0] (notably CPython's
    // sys.executable computation) end up empty, breaking subprocess
    // spawning, `python -m test` worker processes, pip, pytest, etc.
    //
    // Export sensible defaults BEFORE user env exports so user-supplied
    // values (Umlfile [env]) always win.
    s.push_str("# Default environment (must come before user env so they\n");
    s.push_str("# can override). Without PATH set, sh's PATH lookup means\n");
    s.push_str("# argv[0]='python3' (no slash), and CPython's getpath cannot\n");
    s.push_str("# resolve sys.executable — breaking subprocess.Popen,\n");
    s.push_str("# multiprocessing, regrtest workers, pytest, etc.\n");
    s.push_str(
        "export PATH=\"${PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}\"\n",
    );
    s.push_str("export HOME=\"${HOME:-/root}\"\n");
    s.push_str("export TERM=\"${TERM:-linux}\"\n");
    s.push_str("export SHELL=\"${SHELL:-/bin/bash}\"\n");
    s.push_str("\n");

    // Standard pseudo-filesystems and runtime mountpoints. The kernel
    // doesn't auto-mount these for init; without them, common
    // userspace bits silently break:
    //   /proc       — /proc/self/exe, /proc/cpuinfo, etc.
    //   /sys        — sysfs (cgroups, network info, etc.)
    //   /dev/pts    — devpts, required for os.openpty() and TTY tests
    //   /dev/shm    — POSIX shm_open / multiprocessing.shared_memory
    //   /tmp        — tmpfs, ensures fresh writable scratch
    //
    // All "mount; true" so re-runs / hostfs-prepopulated mounts don't
    // fail the script. Errors are silenced to avoid cluttering output.
    s.push_str("# Standard pseudo-filesystems (each mount errors-OK on re-run).\n");
    s.push_str("mount -t proc  proc   /proc    2>/dev/null || true\n");
    s.push_str("mount -t sysfs sysfs  /sys     2>/dev/null || true\n");
    s.push_str("mkdir -p /dev/pts /dev/shm     2>/dev/null || true\n");
    s.push_str("mount -t devpts devpts /dev/pts 2>/dev/null || true\n");
    s.push_str("mount -t tmpfs tmpfs  /dev/shm 2>/dev/null || true\n");
    s.push_str("mount -t tmpfs tmpfs  /tmp     2>/dev/null || true\n");
    s.push_str("\n");

    // Loopback up. Even when network.mode != "tap", many tests +
    // services need lo (binding to localhost, anything that uses 127/8
    // or ::1 for unit-test fixtures). Bring lo up unconditionally; it
    // costs nothing and unblocks ~all networking-touching tests.
    s.push_str("ip link set lo up 2>/dev/null || true\n");
    s.push_str("\n");

    // tmpfs /etc so we can write resolv.conf without touching the
    // host's /etc (under hostfs that file is the host's). The mount
    // is private to this UML instance and goes away on shutdown.
    s.push_str("mount -t tmpfs tmpfs /etc 2>/dev/null || true\n");
    if !uml.network.nameservers.is_empty() {
        s.push_str("cat > /etc/resolv.conf <<'__RESOLV__'\n");
        for ns in &uml.network.nameservers {
            s.push_str(&format!("nameserver {ns}\n"));
        }
        s.push_str("__RESOLV__\n");
    }
    s.push_str("\n");

    // Network up (tap-specific config — lo already up above).
    if uml.network.mode == "tap" {
        let dev = tap_network_plan(&uml.network).guest_dev;

        s.push_str(&format!("ip addr add {} dev {dev}\n", uml.network.guest_ip));
        s.push_str(&format!("ip link set {dev} up\n"));
        s.push_str(&format!(
            "ip route add default via {}\n",
            uml.network.gateway
        ));
        s.push_str("# Give the link a moment to come up before phases run.\n");
        s.push_str("sleep 0.3\n");
        s.push_str("\n");
    }

    // Volumes — bind-mount src → dst. UML uses hostfs as root, so
    // src is reachable as-is; we just `mount --bind` to give the
    // workload a stable in-guest path.
    //
    // The wrinkle: the host's `/` is read-only-ish from inside the
    // guest (UML running as the spawning user, hostfs translates
    // syscalls 1:1, so `mkdir /opt/venv` writes to host's `/opt`
    // which is typically root-owned). We solve this by tmpfs-
    // mounting the IMMEDIATE PARENT of each unique dst path before
    // mkdir + bind. Tmpfs-on-mountpoint shadows whatever was at
    // that path on the host with a fresh writable scratch. Same
    // technique we use for /etc above.
    //
    // Read-only volumes use `mount -o remount,bind,ro` after the
    // bind because Linux ignores the ro flag on the initial bind
    // and requires the remount step to actually flip RW→RO.
    if !uml.volumes.is_empty() {
        s.push_str("# Volume bind-mounts (tmpfs parents to make dst writable\n");
        s.push_str("# under hostfs root; bind-mounts shadow with src content).\n");
        let mut tmpfsed_parents: std::collections::BTreeSet<String> =
            std::collections::BTreeSet::new();
        for v in &uml.volumes {
            let parent = std::path::Path::new(&v.dst)
                .parent()
                .map(|p| p.to_string_lossy().to_string())
                .unwrap_or_else(|| "/".into());
            if parent != "/" && tmpfsed_parents.insert(parent.clone()) {
                s.push_str(&format!(
                    "mount -t tmpfs none {parent} 2>/dev/null || true\n",
                    parent = shell_quote(&parent),
                ));
            }
            s.push_str(&format!("mkdir -p {} 2>/dev/null\n", shell_quote(&v.dst)));
            s.push_str(&format!(
                "mount --bind {} {}\n",
                shell_quote(&v.src),
                shell_quote(&v.dst)
            ));
            if v.mode == "ro" {
                s.push_str(&format!(
                    "mount -o remount,bind,ro {}\n",
                    shell_quote(&v.dst)
                ));
            }
        }
        s.push_str("\n");
    }

    // Env exports.
    if !uml.env.is_empty() {
        s.push_str("# Environment.\n");
        for (k, v) in &uml.env {
            s.push_str(&format!("export {}={}\n", k, shell_quote(v)));
        }
        s.push_str("\n");
    }

    render_network_metadata_exports(&mut s, uml);

    // Phases. Each runs sequentially. The phase command goes through
    // bash's `eval` so users can write pipelines, redirections, &c.
    s.push_str("__umlctl_phase() {\n");
    s.push_str("    local name=\"$1\"; shift\n");
    s.push_str("    echo \"[umlctl phase] $name START\"\n");
    s.push_str("    eval \"$@\"\n");
    s.push_str("    local rc=$?\n");
    s.push_str("    echo \"[umlctl phase] $name END rc=$rc\"\n");
    s.push_str("    return $rc\n");
    s.push_str("}\n\n");

    // Deterministic shutdown via sysrq-trigger.
    //
    // BUG (SMP-T54, 2026-05-04): /sbin/halt on most distros today is a
    // symlink to /bin/systemctl. systemctl-as-halt opens a unix DBus
    // socket to systemd and recvmsg-blocks waiting for a reply. Inside
    // a UML guest where pid=1 is bash (not systemd), the reply never
    // comes — `halt -f` then hangs in recvmsg(NR=47), the kernel RCU
    // stall fires after ~21 jiffies, and the gate-loop counts the run
    // as TIMEOUT instead of PASS. Observed at ~0.5-0.75% per N=400
    // mt-mini SMP soak; the recvmsg-RCU-stall signature was the
    // long-running "init.sh-hang in libc syscall" residual misfiled as
    // a v2 kernel bug.
    //
    // Fix: drop the userspace halt path entirely. `echo b > /proc/sysrq
    // -trigger` invokes the kernel's sysrq_handle_reboot path, which
    // calls machine_restart() immediately — no userspace cooperation
    // and no reliance on pid=1 handling SIGINT (sysrq-`o` would call
    // kill_cad_pid(SIGINT) which only works if init catches SIGINT;
    // bash does not, so sysrq-`o` is unreliable in our setup).
    //
    // Pre-step `echo 1 > /proc/sys/kernel/sysrq` enables all sysrq
    // functions in case the kernel was built with a restrictive mask.
    //
    // No userspace fallback. Adding a fallback like `python3 -c ...
    // reboot()` reintroduces the recvmsg hang via NSS lookups during
    // python3 startup, defeating the purpose. If sysrq isn't compiled
    // in, falling through to `exit 0` triggers the kernel's
    // "Attempted to kill init" panic which still terminates the run
    // deterministically (the gate-loop's exit-on-marker logic is
    // marker-based, not exit-code-based, so a panic-after-REPRO_DONE
    // counts as PASS).
    //
    // Side benefit: user Umlfiles no longer need a trailing `halt -f`
    // phase. Existing ones still work in the success path; in the
    // failure path their halt hangs and the gate-loop times out — to
    // avoid that, drop the halt phase from your Umlfile and let
    // umlctl handle shutdown.
    s.push_str("__umlctl_halt() {\n");
    s.push_str("    sync 2>/dev/null || true\n");
    s.push_str("    echo 1 > /proc/sys/kernel/sysrq 2>/dev/null || true\n");
    s.push_str("    echo b > /proc/sysrq-trigger 2>/dev/null || true\n");
    s.push_str("    # If sysrq is missing, exit and let the kernel panic on\n");
    s.push_str("    # init-exit. Both terminate the run deterministically.\n");
    s.push_str("    exit 0\n");
    s.push_str("}\n\n");

    if uml.init.phases.is_empty() {
        // No phases declared — drop into a shell so the user can poke around.
        s.push_str("echo '[umlctl] no phases declared; dropping into /bin/sh'\n");
        s.push_str("exec /bin/sh\n");
    } else {
        for phase in &uml.init.phases {
            s.push_str(&format!(
                "__umlctl_phase {} {} || {{ echo \"[umlctl] phase {} failed; aborting\"; __umlctl_halt; exit 1; }}\n",
                shell_quote(&phase.name),
                shell_quote(&phase.cmd),
                shell_quote(&phase.name),
            ));
        }
        s.push_str("echo '[umlctl] all phases done'\n");
        s.push_str("__umlctl_halt\n");
    }
    Ok(s)
}

fn render_network_metadata_exports(s: &mut String, uml: &Umlfile) {
    let mut rows = vec![
        ("UMLCTL_NETWORK_MODE", uml.network.mode.clone()),
        ("UMLCTL_NETWORK_DRIVER", "none".to_string()),
        ("UMLCTL_NETDEV", String::new()),
        ("UMLCTL_TAP_NAME", String::new()),
        ("UMLCTL_HOST_IP", String::new()),
        ("UMLCTL_GUEST_IP", String::new()),
        ("UMLCTL_GATEWAY", String::new()),
        ("UMLCTL_NETWORK_TRANSPORT", String::new()),
        ("UMLCTL_NETWORK_HOST_MODE", String::new()),
        ("UMLCTL_NETWORK_QUEUES", "0".to_string()),
    ];

    if uml.network.mode == "tap" {
        let plan = tap_network_plan(&uml.network);
        rows = vec![
            ("UMLCTL_NETWORK_MODE", uml.network.mode.clone()),
            ("UMLCTL_NETWORK_DRIVER", plan.driver),
            ("UMLCTL_NETDEV", plan.guest_dev),
            ("UMLCTL_TAP_NAME", plan.tap_name),
            ("UMLCTL_HOST_IP", uml.network.host_ip.clone()),
            ("UMLCTL_GUEST_IP", uml.network.guest_ip.clone()),
            ("UMLCTL_GATEWAY", uml.network.gateway.clone()),
            ("UMLCTL_NETWORK_TRANSPORT", plan.transport),
            ("UMLCTL_NETWORK_HOST_MODE", plan.host_mode),
            ("UMLCTL_NETWORK_QUEUES", plan.queue_count.to_string()),
        ];
    }

    s.push_str("# umlctl runtime metadata (reserved; describes the generated plan).\n");
    for (key, value) in rows {
        s.push_str(&format!("export {key}={}\n", shell_quote(&value)));
    }
    s.push_str("\n");
}

/// Single-quote-and-escape a string for safe inclusion in bash.
fn shell_quote(s: &str) -> String {
    let mut out = String::from("'");
    for c in s.chars() {
        if c == '\'' {
            out.push_str("'\\''");
        } else {
            out.push(c);
        }
    }
    out.push('\'');
    out
}

/// Parse "10.7.0.2/24" → "10.7.0.0/24". Used for the MASQUERADE
/// source rule.
fn guest_cidr(guest_ip: &str) -> Result<String> {
    let (ip, prefix) = guest_ip
        .split_once('/')
        .ok_or_else(|| anyhow!("guest_ip must be A.B.C.D/N (got {:?})", guest_ip))?;
    let prefix: u8 = prefix
        .parse()
        .with_context(|| format!("guest_ip prefix {:?}", prefix))?;
    if prefix > 32 {
        bail!("guest_ip prefix must be 0..=32 (got {})", prefix);
    }
    let octets: Vec<u8> = ip
        .split('.')
        .map(|o| o.parse::<u8>().context("bad ipv4 octet"))
        .collect::<Result<_>>()?;
    if octets.len() != 4 {
        bail!("guest_ip must be IPv4 (got {:?})", guest_ip);
    }
    let host_bits = 32u32.saturating_sub(prefix as u32);
    let mask: u32 = if host_bits == 32 {
        0
    } else {
        !0u32 << host_bits
    };
    let v = ((octets[0] as u32) << 24)
        | ((octets[1] as u32) << 16)
        | ((octets[2] as u32) << 8)
        | (octets[3] as u32);
    let net = v & mask;
    Ok(format!(
        "{}.{}.{}.{}/{}",
        (net >> 24) & 0xff,
        (net >> 16) & 0xff,
        (net >> 8) & 0xff,
        net & 0xff,
        prefix
    ))
}

/// Strip the prefix from "10.7.0.2/24" → "10.7.0.2".
fn guest_ip_addr(guest_ip: &str) -> Result<String> {
    Ok(guest_ip
        .split('/')
        .next()
        .ok_or_else(|| anyhow!("empty guest_ip"))?
        .to_string())
}

/// Detect the host's default-route iface (`ip -o route show default`).
fn detect_default_iface() -> Result<String> {
    let out = Command::new("ip")
        .args(["-o", "route", "show", "default"])
        .output()
        .context("run `ip -o route show default`")?;
    if !out.status.success() {
        bail!(
            "`ip route` failed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
    }
    let s = String::from_utf8_lossy(&out.stdout);
    // Parse "default via 192.168.1.1 dev enp3s0 proto dhcp src ..."
    for tok in s.split_whitespace().enumerate() {
        if tok.1 == "dev" {
            if let Some((_, name)) = s.split_whitespace().enumerate().nth(tok.0 + 1) {
                return Ok(name.to_string());
            }
        }
    }
    bail!(
        "could not parse default-route iface from `ip route` output: {:?}",
        s
    );
}

/// Run a list of `sh -c` steps via sudo, stopping on first failure.
/// Used by `up` (setup_steps) and `down` (teardown_steps; non-fatal).
pub fn run_sudo_steps(steps: &[String], stop_on_failure: bool, quiet: bool) -> Result<()> {
    for cmd in steps {
        if !quiet {
            eprintln!("[umlctl] sudo: {cmd}");
        }
        let st = Command::new("sudo")
            .args(["sh", "-c", cmd])
            .status()
            .with_context(|| format!("spawn sudo sh -c {:?}", cmd))?;
        if !st.success() {
            if stop_on_failure {
                bail!("sudo step failed (rc={:?}): {}", st.code(), cmd);
            } else if !quiet {
                eprintln!("[umlctl] (teardown step rc={:?}, continuing)", st.code());
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_roundtrip_minimal() {
        let s = r#"
schema_version = 1
[instance]
name = "fastapi-demo"
[kernel]
path = "/tmp/uml-clean/linux"
"#;
        let u: Umlfile = toml::from_str(s).unwrap();
        assert_eq!(u.instance.name, "fastapi-demo");
        assert_eq!(u.kernel.backend, "seccomp");
        assert_eq!(u.runtime.mem, "512M");
        assert_eq!(u.network.mode, "none");
        assert_eq!(u.network.driver, "vector");
    }

    #[test]
    fn unknown_field_rejected() {
        let s = r#"
schema_version = 1
[instance]
name = "x"
typo_field = "oops"
[kernel]
path = "/x"
"#;
        let r: Result<Umlfile, _> = toml::from_str(s);
        assert!(r.is_err(), "deny_unknown_fields should reject typos");
    }

    #[test]
    fn port_forward_parses() {
        assert_eq!(
            parse_port_forward("8765:8765/tcp").unwrap(),
            (8765, 8765, "tcp".into())
        );
        assert_eq!(
            parse_port_forward("80:8080").unwrap(),
            (80, 8080, "tcp".into())
        );
        assert_eq!(
            parse_port_forward("53:53/udp").unwrap(),
            (53, 53, "udp".into())
        );
        assert!(parse_port_forward("oops").is_err());
        assert!(parse_port_forward("80:8080/sctp").is_err());
    }

    #[test]
    fn guest_cidr_masks_correctly() {
        assert_eq!(guest_cidr("10.7.0.2/24").unwrap(), "10.7.0.0/24");
        assert_eq!(guest_cidr("192.168.1.42/16").unwrap(), "192.168.0.0/16");
        assert_eq!(guest_cidr("10.0.0.5/8").unwrap(), "10.0.0.0/8");
        assert!(guest_cidr("10.7.0.2").is_err());
        assert!(guest_cidr("10.7.0.2/40").is_err());
    }

    #[test]
    fn shell_quote_escapes_singletons() {
        assert_eq!(shell_quote("hello"), "'hello'");
        assert_eq!(shell_quote("it's"), "'it'\\''s'");
        assert_eq!(shell_quote(""), "''");
    }

    #[test]
    fn render_init_includes_phases() {
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
[[init.phases]]
name = "hello"
cmd = "echo hi"
"#,
        )
        .unwrap();
        let s = render_init_script(&u).unwrap();
        assert!(s.contains("nameserver 8.8.8.8"));
        assert!(s.contains("ip addr add 10.7.0.2/24 dev vec0"));
        assert!(s.contains("export UMLCTL_NETWORK_DRIVER='vector'"));
        assert!(s.contains("export UMLCTL_NETDEV='vec0'"));
        assert!(tap_network_plan(&u.network).kernel_arg.starts_with("vec0:"));
        assert!(s.contains("__umlctl_phase 'hello' 'echo hi'"));
    }

    #[test]
    fn vector2_network_driver_renders_vec2_device() {
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector2"
tap_name = "soak-tap0"
"#,
        )
        .unwrap();

        let s = render_init_script(&u).unwrap();

        assert!(s.contains("ip addr add 10.7.0.2/24 dev vec2.0"));
        assert!(s.contains("ip link set vec2.0 up"));
        assert!(s.contains("export UMLCTL_NETWORK_DRIVER='vector2'"));
        assert!(s.contains("export UMLCTL_NETDEV='vec2.0'"));
        assert!(s.contains("export UMLCTL_TAP_NAME='soak-tap0'"));
        assert!(s.contains("export UMLCTL_NETWORK_HOST_MODE='inproc'"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUES='1'"));
        let plan = tap_network_plan(&u.network);
        assert_eq!(plan.driver, "vector2");
        assert_eq!(plan.guest_dev, "vec2.0");
        assert_eq!(plan.host_mode, "inproc");
        assert_eq!(plan.queue_count, 1);
        assert_eq!(
            plan.kernel_arg,
            "vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128",
        );
    }

    #[test]
    fn set_network_driver_validates_mode() {
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
"#,
        )
        .unwrap();

        set_network_driver(&mut u, "vector2").unwrap();
        assert_eq!(u.network.driver, "vector2");

        u.network.mode = "none".into();
        assert!(set_network_driver(&mut u, "vector2").is_err());
        assert!(set_network_driver(&mut u, "bogus").is_err());
    }

    #[test]
    fn network_driver_rejects_unknown_value() {
        let r: Result<Umlfile, _> = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "bogus"
"#,
        );
        assert!(r.is_ok());

        let mut u = r.unwrap();
        let tmp = tempfile::NamedTempFile::new().unwrap();
        let path = tmp.path().to_path_buf();
        std::fs::write(&path, toml::to_string(&u).unwrap()).unwrap();
        assert!(Umlfile::from_path(&path).is_err());

        u.network.driver = "vector2".into();
        u.network.mode = "none".into();
        std::fs::write(&path, toml::to_string(&u).unwrap()).unwrap();
        assert!(Umlfile::from_path(&path).is_err());
    }

    #[test]
    fn render_init_exports_none_network_metadata() {
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "minimal"
[kernel]
path = "/x"
[network]
mode = "none"
"#,
        )
        .unwrap();
        let s = render_init_script(&u).unwrap();

        assert!(s.contains("export UMLCTL_NETWORK_MODE='none'"));
        assert!(s.contains("export UMLCTL_NETWORK_DRIVER='none'"));
        assert!(s.contains("export UMLCTL_NETDEV=''"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUES='0'"));
    }

    /// Init script ALWAYS exports default env even with no [env] section.
    /// Required for CPython's sys.executable, subprocess workers, etc.
    /// Regression guard: if these get accidentally removed, `python -m test`
    /// stops working.
    #[test]
    fn render_init_always_exports_default_env() {
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "minimal"
[kernel]
path = "/x"
[network]
mode = "none"
"#,
        )
        .unwrap();
        let s = render_init_script(&u).unwrap();
        // PATH default — required for CPython sys.executable resolution.
        assert!(s.contains("export PATH=\"${PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}\""),
                "missing default PATH export");
        // HOME, TERM, SHELL fallbacks.
        assert!(s.contains("export HOME="), "missing HOME export");
        assert!(s.contains("export TERM="), "missing TERM export");
        assert!(s.contains("export SHELL="), "missing SHELL export");
    }

    /// Init script ALWAYS mounts standard pseudo-FS even in minimal config.
    /// Regression guard: tests doing os.openpty(), bind to localhost, /proc
    /// access, /dev/shm POSIX shm, etc. depend on these.
    #[test]
    fn render_init_always_mounts_pseudo_fs() {
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "minimal"
[kernel]
path = "/x"
[network]
mode = "none"
"#,
        )
        .unwrap();
        let s = render_init_script(&u).unwrap();
        assert!(s.contains("mount -t proc"), "missing /proc mount");
        assert!(s.contains("mount -t sysfs"), "missing /sys mount");
        assert!(s.contains("mount -t devpts"), "missing /dev/pts mount");
        assert!(
            s.contains("mount -t tmpfs tmpfs  /dev/shm"),
            "missing /dev/shm mount"
        );
        assert!(
            s.contains("mount -t tmpfs tmpfs  /tmp"),
            "missing /tmp mount"
        );
    }

    /// Loopback brought up unconditionally regardless of network mode.
    /// Tests binding to localhost (test_docxmlrpc, test_external_inspection,
    /// test_asyncio, etc.) need this even when mode = "none".
    #[test]
    fn render_init_always_brings_lo_up() {
        for mode in ["none", "tap"] {
            let toml_str = format!(
                r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "{}"
"#,
                mode
            );
            let u: Umlfile = toml::from_str(&toml_str).unwrap();
            let s = render_init_script(&u).unwrap();
            assert!(
                s.contains("ip link set lo up"),
                "loopback not brought up for mode={}",
                mode
            );
        }
    }

    /// Default env can be overridden by [env] section. Order matters:
    /// defaults must come BEFORE user env so user values win.
    #[test]
    fn render_init_user_env_overrides_defaults() {
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "none"
[env]
PATH = "/custom/bin"
HOME = "/customhome"
"#,
        )
        .unwrap();
        let s = render_init_script(&u).unwrap();
        // The default export is unconditional and uses ${PATH:-...} so
        // it doesn't override an already-set PATH. The [env] section
        // exports come later and use plain `export X=...` so they win.
        let default_pos = s
            .find("export PATH=\"${PATH:-")
            .expect("default PATH missing");
        let user_pos = s
            .find("export PATH='/custom/bin'")
            .expect("user PATH missing");
        assert!(
            default_pos < user_pos,
            "default PATH must come before user [env] PATH"
        );
    }
}
