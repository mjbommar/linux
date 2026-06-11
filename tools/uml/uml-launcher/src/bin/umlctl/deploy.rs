// SPDX-License-Identifier: GPL-2.0
//
// umlctl `up`/`down` - declarative UML deployment from a single
// TOML "Umlfile". Inspired by docker-compose.yml: one file
// describes kernel, network, ports, mounts, env, init phases.
//
// The promise: `umlctl up -f Umlfile.toml` brings a working
// service up end-to-end (TAP+NAT+port-forward+resolv.conf+
// in-guest networking+the actual workload) without the user
// hand-rolling iptables and init scripts. `umlctl down` tears
// it down cleanly.
//
// This exists so every path name, network detail, and port forward is
// declared once instead of being re-derived by shell scripts around
// the low-level lifecycle verbs.
//
// Keep this file additive - the existing `create`/`start`/
// `stop` verbs stay as the low-level primitives; `up`/`down`
// are sugar that compiles an Umlfile down to a manifest +
// host-side setup steps + a generated init script.

use anyhow::{anyhow, bail, Context, Result};
use serde::de::Error as DeError;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use std::collections::BTreeMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::manifest;

/// Schema version of the Umlfile format. Bumped when wire-format
/// breaks. Today only v1.
const UMLFILE_SCHEMA_VERSION: u32 = 1;
pub const VECTOR2_TAP_FD: i32 = 200;
pub const MAX_NETWORK_QUEUES: u32 = 1024;

pub const LABEL_NETWORK_MODE: &str = "umlctl.network.mode";
pub const LABEL_NETWORK_DRIVER: &str = "umlctl.network.driver";
pub const LABEL_NETWORK_GUEST_DEV: &str = "umlctl.network.guest_dev";
pub const LABEL_NETWORK_TAP_NAME: &str = "umlctl.network.tap_name";
pub const LABEL_NETWORK_TRANSPORT: &str = "umlctl.network.transport";
pub const LABEL_NETWORK_HOST_MODE: &str = "umlctl.network.host_mode";
pub const LABEL_NETWORK_QUEUES: &str = "umlctl.network.queues";
pub const LABEL_NETWORK_QUEUE_SPEC: &str = "umlctl.network.queue_spec";
pub const LABEL_NETWORK_FD: &str = "umlctl.network.fd";
pub const LABEL_NETWORK_FD_COUNT: &str = "umlctl.network.fd_count";
pub const LABEL_NETWORK_FAIL_OPEN_AFTER: &str = "umlctl.network.fail_open_after";

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
    /// Host-process resource controls.
    /// Optional - empty section -> no env vars set, no cgroup created.
    #[serde(default)]
    pub host_resources: HostResourcesSection,
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
    /// Fast-boot mode (Firecracker-class boot latency).  When true,
    /// umlctl appends `quiet lpj=<calibrated>` to the kernel cmdline
    /// and skips non-essential boot output.  Measured ~207 ms boot on
    /// the server7 reference host with mem=64M ncpus=1.
    ///
    /// The lpj value is sniffed from the host's
    /// /proc/cpuinfo BogoMIPS at deploy time (BogoMIPS x 1e6 / 2 ~=
    /// loops_per_jiffy at HZ=1000); a missing/unparseable cpuinfo
    /// falls back to no lpj=, which gives the standard ~300 ms boot
    /// with calibration jitter.
    #[serde(default)]
    pub fast_boot: bool,
    /// Root filesystem source.  Defaults to "hostfs": the host's `/`
    /// is bind-mounted as the guest's root via hostfs, and umlctl
    /// synthesizes an init.sh that runs the init.phases pipeline.
    ///
    /// Set to "ubd" when the Umlfile was emitted by `umlbuild instance`
    /// - the kernel boots from a ubd-attached ext4 image specified by
    /// `kernel.append`, and umlctl skips its init.sh synthesis (the
    /// rootfs ships its own /sbin/init).  Any other value is passed
    /// through verbatim as `root=<value>` (e.g. "/dev/ubdb").
    #[serde(default = "default_root")]
    pub root: String,
}

fn default_root() -> String {
    "hostfs".to_string()
}

impl Default for RuntimeSection {
    fn default() -> Self {
        Self {
            mem: "512M".into(),
            ncpus: 1,
            fast_boot: false,
            root: default_root(),
        }
    }
}

#[derive(Serialize, Debug, Clone)]
pub struct NetworkSection {
    /// "none" (default) or "tap".
    pub mode: String,
    /// "vector" for vec0 or "vector2" for vec2.0.
    pub driver: String,
    /// "auto", "fd", or "inproc". For vector2, auto chooses the
    /// launcher-owned inherited-fd TAP path.
    pub host_mode: String,
    /// Number of guest/host queues. `auto` resolves to runtime.ncpus.
    /// Values above 1, and `auto`, require vector2.
    pub queues: NetworkQueueSpec,
    /// Optional vector2 fault-injection threshold for live open-unwind
    /// validation. `N` fails the Nth and later netdev opens.
    pub fail_open_after: Option<u32>,
    pub tap_name: String,
    pub guest_ip: String,
    pub host_ip: String,
    pub gateway: String,
    pub nameservers: Vec<String>,
    /// "auto" picks the host's default-route iface; or e.g. "enp3s0".
    pub masquerade_via: String,
    /// Each entry: "host_port:guest_port[/proto]" - proto defaults
    /// to tcp. Implemented via host-side DNAT to guest_ip:guest_port.
    pub ports: Vec<String>,
    /// Set to true by the TOML deserializer when `driver` was present
    /// in the source file (vs. serde-defaulted to "vector2").
    /// Validator uses this to distinguish "user explicitly chose a
    /// driver while mode=none" (error) from "user left it unset and
    /// the default flowed in" (fine).  Skipped on serialize so
    /// round-tripped manifests don't accumulate this metadata.
    #[serde(skip)]
    pub driver_explicit: bool,
}

/* Manual Deserialize so we can track whether `driver` was present in
 * the source TOML.  serde's #[serde(default)] flatten makes it
 * impossible to distinguish "field absent" from "field present with
 * default-equal value".
 * The wrapper struct uses Option<String> for driver, then resolve()
 * fills in the post-flip default ("vector2") and sets
 * driver_explicit accordingly.
 */
impl<'de> serde::Deserialize<'de> for NetworkSection {
    fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(deny_unknown_fields, default)]
        struct Raw {
            mode: String,
            driver: Option<String>,
            host_mode: String,
            queues: NetworkQueueSpec,
            fail_open_after: Option<u32>,
            tap_name: String,
            guest_ip: String,
            host_ip: String,
            gateway: String,
            nameservers: Vec<String>,
            masquerade_via: String,
            ports: Vec<String>,
        }
        impl Default for Raw {
            fn default() -> Self {
                let d = NetworkSection::default();
                Self {
                    mode: d.mode,
                    driver: None,
                    host_mode: d.host_mode,
                    queues: d.queues,
                    fail_open_after: d.fail_open_after,
                    tap_name: d.tap_name,
                    guest_ip: d.guest_ip,
                    host_ip: d.host_ip,
                    gateway: d.gateway,
                    nameservers: d.nameservers,
                    masquerade_via: d.masquerade_via,
                    ports: d.ports,
                }
            }
        }
        let r = Raw::deserialize(deserializer)?;
        let (driver, driver_explicit) = match r.driver {
            Some(d) => (d, true),
            None => ("vector2".to_string(), false),
        };
        Ok(NetworkSection {
            mode: r.mode,
            driver,
            host_mode: r.host_mode,
            queues: r.queues,
            fail_open_after: r.fail_open_after,
            tap_name: r.tap_name,
            guest_ip: r.guest_ip,
            host_ip: r.host_ip,
            gateway: r.gateway,
            nameservers: r.nameservers,
            masquerade_via: r.masquerade_via,
            ports: r.ports,
            driver_explicit,
        })
    }
}

#[derive(Debug, Clone)]
pub struct NetworkPlan {
    pub driver: String,
    pub guest_dev: String,
    pub tap_name: String,
    pub transport: String,
    pub host_mode: String,
    pub queue_spec: String,
    pub queue_count: u32,
    pub fail_open_after: Option<u32>,
    pub kernel_arg: String,
    pub inherited_fd: Option<i32>,
    pub inherited_fd_count: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NetworkQueueSpec {
    Fixed(u32),
    Auto,
}

impl Default for NetworkQueueSpec {
    fn default() -> Self {
        Self::Fixed(1)
    }
}

impl NetworkQueueSpec {
    pub fn label(&self) -> String {
        match self {
            Self::Fixed(queues) => queues.to_string(),
            Self::Auto => "auto".to_string(),
        }
    }

    pub fn is_auto(&self) -> bool {
        matches!(self, Self::Auto)
    }
}

impl std::str::FromStr for NetworkQueueSpec {
    type Err = anyhow::Error;

    fn from_str(raw: &str) -> Result<Self> {
        if raw.eq_ignore_ascii_case("auto") {
            return Ok(Self::Auto);
        }
        let queues = raw
            .parse::<u32>()
            .with_context(|| format!("parse network.queues={raw:?}"))?;
        if queues == 0 {
            bail!("network.queues must be >= 1");
        }
        if queues > MAX_NETWORK_QUEUES {
            bail!("network.queues must be <= {MAX_NETWORK_QUEUES}");
        }
        Ok(Self::Fixed(queues))
    }
}

impl std::fmt::Display for NetworkQueueSpec {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.label())
    }
}

impl Serialize for NetworkQueueSpec {
    fn serialize<S>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            Self::Fixed(queues) => serializer.serialize_u32(*queues),
            Self::Auto => serializer.serialize_str("auto"),
        }
    }
}

impl<'de> Deserialize<'de> for NetworkQueueSpec {
    fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum RawQueueSpec {
            Fixed(u32),
            Named(String),
        }

        match RawQueueSpec::deserialize(deserializer)? {
            RawQueueSpec::Fixed(queues) => Ok(Self::Fixed(queues)),
            RawQueueSpec::Named(name) if name.eq_ignore_ascii_case("auto") => Ok(Self::Auto),
            RawQueueSpec::Named(name) => Err(D::Error::custom(format!(
                "network.queues must be an integer or 'auto' (got {name:?})"
            ))),
        }
    }
}

impl Default for NetworkSection {
    fn default() -> Self {
        Self {
            mode: "none".into(),
            // vector2 is the default for umlctl-managed TAP/fd
            // networking. Users who need vec0 or legacy-only
            // transports can opt back via `[network].driver = "vector"`.
            driver: "vector2".into(),
            host_mode: "auto".into(),
            queues: NetworkQueueSpec::default(),
            fail_open_after: None,
            tap_name: "uml-tap0".into(),
            guest_ip: "10.7.0.2/24".into(),
            host_ip: "10.7.0.1/24".into(),
            gateway: "10.7.0.1".into(),
            nameservers: vec!["8.8.8.8".into(), "1.1.1.1".into()],
            masquerade_via: "auto".into(),
            ports: Vec::new(),
            driver_explicit: false,
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

/// Host-process resource controls.  An empty section leaves the host
/// process unmanaged.
///
/// Translated into env vars (UM_THP / UM_OOM_SCORE_ADJ /
/// UM_KVM_V2_CPU_AFFINITY / UM_HUGEPAGES / UM_KVM_V2_PIN_PHYSMEM)
/// at UML spawn time. Cgroup v2 limits (memory_max / cpu_max /
/// pids_max) are written to a per-instance cgroup before exec.
#[derive(Serialize, Deserialize, Debug, Clone, Default)]
#[serde(deny_unknown_fields, default)]
pub struct HostResourcesSection {
    /// Transparent-huge-page policy on physmem.
    /// "off" -> MADV_NOHUGEPAGE (predictable, no defrag latency).
    /// "on"  -> MADV_HUGEPAGE (throughput-oriented).
    /// "auto" / "" -> inherit system default.
    pub thp: String,

    /// OOM score adjustment. None -> no write to
    /// /proc/self/oom_score_adj. Some(n) -> write n (range
    /// [-1000, +1000], host clamps out-of-range).
    pub oom_score_adj: Option<i32>,

    /// Process-level CPU affinity. CPU list with optional
    /// ranges, e.g. "0-3" / "0,2,4" / "0-1,4-5". Empty -> no
    /// sched_setaffinity call (inherit current mask).
    pub cpu_affinity: String,

    /// Per-vCPU host-thread affinity.  "auto" / "off" / "<list>".
    ///
    ///   "auto"   each vCPU host thread bound to one host CPU
    ///            within @cpu_affinity (vCPU N -> cpu_set[N %
    ///            len(cpu_set)]).
    ///   "off"    no thread-level pinning (current behaviour;
    ///            threads inherit @cpu_affinity).
    ///   "<list>" explicit per-vCPU CPU list e.g. "0,1,4,5".
    ///
    /// On kvm-v2 hosts most of the per-vCPU stickiness is already
    /// enforced by the per-host-CPU vCPU pool design (vcpus[N] is
    /// by construction only used from smp_processor_id() == N).
    /// This field is the lever for layering an additional
    /// sched_setaffinity call on the guest tasks that drive
    /// KVM_RUN on each pool entry, useful when a NUMA host wants
    /// the guest's userspace work tied to the same socket as the
    /// physmem.  Implementation lives in kvm-v2 backend; this
    /// section just plumbs the value into env vars.
    pub vcpu_thread_affinity: String,

    /// Hugepage backing for physmem.
    /// "2M" -> MAP_HUGETLB | MAP_HUGE_2MB.
    /// "1G" -> MAP_HUGETLB | MAP_HUGE_1GB.
    /// "off" / "" -> 4 KiB.
    /// Requires the host hugetlbfs pool to be pre-reserved via
    /// /proc/sys/vm/nr_hugepages. Empty pool -> 4 KiB fallback +
    /// pr_warn at boot.
    pub hugepages: String,

    /// MAP_LOCKED on physmem.  Foundation pin/populate is always
    /// active; setting this true additionally applies MAP_LOCKED on
    /// the physmem mapping (requires cap_ipc_lock setcap on the UML
    /// binary).
    pub pin_physmem: bool,

    /// cgroup v2 memory.max. "1G" / "512M" / "0" (unlimited).
    /// Empty -> no cgroup memory limit set.
    pub memory_max: String,

    /// cgroup v2 cpu.max. "200%" (two full cores) /
    /// "100ms 100ms" (raw cgroup v2 form). Empty -> no cgroup
    /// cpu limit.
    pub cpu_max: String,

    /// cgroup v2 pids.max. Empty -> no pids limit.
    pub pids_max: Option<u32>,
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
    /// an init phase fails - useful for poking around with `umlctl
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
                    "volume {} -> {}: mode must be 'ro' or 'rw' (got {:?})",
                    v.src,
                    v.dst,
                    v.mode
                );
            }
        }
        validate_runtime_section(&u.runtime)?;
        validate_network_section(&u.network, &u.runtime)?;
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
    let old = uml.network.driver.clone();
    let old_explicit = uml.network.driver_explicit;
    uml.network.driver = driver.to_string();
    // Set by CLI / sweep / programmatic mutation; validation treats
    // this the same as an explicit TOML field.
    uml.network.driver_explicit = true;
    if let Err(e) = validate_network_section(&uml.network, &uml.runtime) {
        uml.network.driver = old;
        uml.network.driver_explicit = old_explicit;
        return Err(e);
    }
    Ok(())
}

pub fn set_network_queue_spec(uml: &mut Umlfile, queues: NetworkQueueSpec) -> Result<()> {
    let old = uml.network.queues.clone();
    uml.network.queues = queues;
    if let Err(e) = validate_network_section(&uml.network, &uml.runtime) {
        uml.network.queues = old;
        return Err(e);
    }
    Ok(())
}

pub fn set_network_host_mode(uml: &mut Umlfile, host_mode: &str) -> Result<()> {
    validate_network_host_mode(host_mode)?;
    let old = uml.network.host_mode.clone();
    uml.network.host_mode = host_mode.to_string();
    if let Err(e) = validate_network_section(&uml.network, &uml.runtime) {
        uml.network.host_mode = old;
        return Err(e);
    }
    Ok(())
}

pub fn resolve_network_queues(net: &NetworkSection, runtime: &RuntimeSection) -> Result<u32> {
    match net.queues {
        NetworkQueueSpec::Fixed(queues) => validate_network_queue_count(queues),
        NetworkQueueSpec::Auto => {
            validate_runtime_section(runtime)?;
            validate_network_queue_count(runtime.ncpus).with_context(|| {
                format!(
                    "resolve network.queues='auto' from runtime.ncpus={}",
                    runtime.ncpus
                )
            })
        }
    }
}

pub fn network_plan_labels(plan: &NetworkPlan) -> Vec<String> {
    let mut labels = vec![
        format!("{LABEL_NETWORK_MODE}=tap"),
        format!("{LABEL_NETWORK_DRIVER}={}", plan.driver),
        format!("{LABEL_NETWORK_GUEST_DEV}={}", plan.guest_dev),
        format!("{LABEL_NETWORK_TAP_NAME}={}", plan.tap_name),
        format!("{LABEL_NETWORK_TRANSPORT}={}", plan.transport),
        format!("{LABEL_NETWORK_HOST_MODE}={}", plan.host_mode),
        format!("{LABEL_NETWORK_QUEUES}={}", plan.queue_count),
        format!("{LABEL_NETWORK_QUEUE_SPEC}={}", plan.queue_spec),
    ];
    if let Some(fail_open_after) = plan.fail_open_after {
        labels.push(format!("{LABEL_NETWORK_FAIL_OPEN_AFTER}={fail_open_after}"));
    }
    if let Some(fd) = plan.inherited_fd {
        labels.push(format!("{LABEL_NETWORK_FD}={fd}"));
        labels.push(format!(
            "{LABEL_NETWORK_FD_COUNT}={}",
            plan.inherited_fd_count
        ));
    }
    labels
}

fn validate_runtime_section(runtime: &RuntimeSection) -> Result<()> {
    if runtime.ncpus == 0 {
        bail!("runtime.ncpus must be >= 1");
    }
    Ok(())
}

fn validate_network_queue_count(queues: u32) -> Result<u32> {
    if queues == 0 {
        bail!("network.queues must be >= 1");
    }
    if queues > MAX_NETWORK_QUEUES {
        bail!("network.queues must be <= {MAX_NETWORK_QUEUES}");
    }
    Ok(queues)
}

fn validate_network_section(net: &NetworkSection, runtime: &RuntimeSection) -> Result<()> {
    if !["none", "tap"].contains(&net.mode.as_str()) {
        bail!("network.mode must be 'none' or 'tap' (got {:?})", net.mode);
    }
    validate_network_driver(&net.driver)?;
    validate_network_host_mode(&net.host_mode)?;
    // network.driver is informational when mode != "tap": the driver
    // only ever does work in tap mode.  Distinguish an explicit
    // driver with mode=none (error) from an absent field that defaulted
    // to vector2 (harmless).
    // The driver_explicit flag is populated by the manual Deserialize
    // impl above.  Unknown driver names are still caught by
    // validate_network_driver() above regardless of mode.
    if net.mode != "tap" && net.driver_explicit {
        bail!(
            "network.driver = {:?} is only meaningful when \
             network.mode = 'tap'; drop the line if you want \
             the default driver to flow through silently",
            net.driver
        );
    }
    if net.mode != "tap" && net.host_mode != "auto" {
        bail!("network.host_mode is only meaningful when network.mode = 'tap'");
    }
    let queues = resolve_network_queues(net, runtime)?;
    if net.mode != "tap" && (queues != 1 || net.queues.is_auto()) {
        bail!("network.queues is only meaningful when network.mode = 'tap'");
    }
    if net.driver != "vector2" && (queues != 1 || net.queues.is_auto()) {
        bail!("network.queues > 1 or 'auto' requires network.driver = 'vector2'");
    }
    if net.driver != "vector2" && net.host_mode == "fd" {
        bail!("network.host_mode = 'fd' requires network.driver = 'vector2'");
    }
    if let Some(fail_open_after) = net.fail_open_after {
        if fail_open_after == 0 {
            bail!("network.fail_open_after must be >= 1");
        }
        if net.driver != "vector2" {
            bail!("network.fail_open_after requires network.driver = 'vector2'");
        }
    }
    Ok(())
}

pub fn validate_network_host_mode(host_mode: &str) -> Result<()> {
    if !["auto", "fd", "inproc"].contains(&host_mode) {
        bail!(
            "network.host_mode must be 'auto', 'fd', or 'inproc' (got {:?})",
            host_mode
        );
    }
    Ok(())
}

/// Expand $VAR / ${VAR} / $HOME from process env. Unknown vars
/// expand to empty (not an error - gives us "kernel.path = $UML_KERNEL"
/// usability without per-Umlfile gating).
fn expand_env(s: &str) -> String {
    shellexpand::env(s)
        .map(|c| c.to_string())
        .unwrap_or_else(|_| s.to_string())
}

/// Parse "8765:8765/tcp" -> (host=8765, guest=8765, proto="tcp").
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

fn tap_network_plan(net: &NetworkSection, runtime: &RuntimeSection) -> Result<NetworkPlan> {
    let queue_count = resolve_network_queues(net, runtime)?;
    let queue_spec = net.queues.label();
    let fail_open_arg = net
        .fail_open_after
        .map(|attempt| format!(",fail_open_after={attempt}"))
        .unwrap_or_default();
    match net.driver.as_str() {
        "vector2" => {
            let host_mode = match net.host_mode.as_str() {
                "fd" => "fd",
                "inproc" => "inproc",
                _ => "fd",
            };
            if host_mode == "fd" {
                let queue_arg = if queue_count > 1 {
                    format!(",queues={queue_count}")
                } else {
                    String::new()
                };
                Ok(NetworkPlan {
                    driver: "vector2".into(),
                    guest_dev: "vec2.0".into(),
                    tap_name: net.tap_name.clone(),
                    transport: "fd".into(),
                    host_mode: "fd".into(),
                    queue_spec,
                    queue_count,
                    fail_open_after: net.fail_open_after,
                    kernel_arg: format!(
                        // gso=on + csum=on are safe with the fd-handoff
                        // shape - tapfd.rs opens /dev/net/tun with
                        // IFF_VNET_HDR + TUNSETOFFLOAD(TUN_F_CSUM | TSO*),
                        // so the kernel's TCP stack hands large GSO skbs
                        // down and `virtio_net_hdr_from_skb` encodes the
                        // gso_type for the host to segment, reducing
                        // per-MTU-frame syscall cost.
                        "vec2.0:transport=fd,mode=fd,fd={fd},depth=128,gso=1,csum=1{queue_arg}{fail_open_arg}",
                        fd = VECTOR2_TAP_FD,
                        queue_arg = queue_arg,
                        fail_open_arg = fail_open_arg,
                    ),
                    inherited_fd: Some(VECTOR2_TAP_FD),
                    inherited_fd_count: queue_count,
                })
            } else {
                let queue_arg = if queue_count > 1 {
                    format!(",queues={queue_count}")
                } else {
                    String::new()
                };
                Ok(NetworkPlan {
                    driver: "vector2".into(),
                    guest_dev: "vec2.0".into(),
                    tap_name: net.tap_name.clone(),
                    transport: "tap".into(),
                    host_mode: "inproc".into(),
                    queue_spec,
                    queue_count,
                    fail_open_after: net.fail_open_after,
                    kernel_arg: format!(
                        "vec2.0:transport=tap,mode=inproc,ifname={tap},depth=128{queue_arg}{fail_open_arg}",
                        tap = net.tap_name,
                        queue_arg = queue_arg,
                        fail_open_arg = fail_open_arg,
                    ),
                    inherited_fd: None,
                    inherited_fd_count: 0,
                })
            }
        }
        _ => Ok(NetworkPlan {
            driver: "vector".into(),
            guest_dev: "vec0".into(),
            tap_name: net.tap_name.clone(),
            transport: "tap".into(),
            host_mode: "legacy-inproc".into(),
            queue_spec,
            queue_count: 1,
            fail_open_after: None,
            kernel_arg: format!(
                "vec0:transport=tap,ifname={tap},depth=128",
                tap = net.tap_name,
            ),
            inherited_fd: None,
            inherited_fd_count: 0,
        }),
    }
}

// -------------------------------------------------------------------
// Compile: Umlfile -> (host setup actions, init script, kernel cmdline)
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

    if uml.runtime.fast_boot {
        // Fast-boot mode (~207 ms vs ~308 ms baseline on the reference
        // host).  `quiet` cuts the kernel-printk console writes that
        // would otherwise serialize on the host stderr (write() to a
        // potentially-pipe-blocked fd inside a critical boot path).
        // `lpj=<sniffed>` skips the BogoMIPS calibration loop that
        // normally rounds up to the next jiffy and adds ~100 ms of
        // wall-clock jitter at HZ=1000.
        append.push("quiet".into());
        if let Some(lpj) = sniff_host_lpj() {
            append.push(format!("lpj={lpj}"));
        }
    }

    let mut setup_steps = Vec::new();
    let mut teardown_steps = Vec::new();
    let mut network_plan = None;

    if uml.network.mode == "tap" {
        let net = &uml.network;
        let plan = tap_network_plan(net, &uml.runtime)?;
        let masq_iface = if net.masquerade_via == "auto" {
            detect_default_iface().unwrap_or_else(|_| "eth0".into())
        } else {
            net.masquerade_via.clone()
        };

        // Setup: ip tuntap add -> addr -> up -> forwarding -> MASQUERADE -> port-forwards
        let user = std::env::var("USER").unwrap_or_else(|_| "uml".into());
        let multi_queue = if plan.driver == "vector2" && plan.queue_count > 1 {
            " multi_queue"
        } else {
            ""
        };
        setup_steps.push(format!(
            "ip tuntap add dev {tap} mode tap user {user}{multi_queue}",
            tap = net.tap_name,
            user = user,
            multi_queue = multi_queue,
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
            // reply path (uml-tap0 -> host) is symmetric.
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
            "ip tuntap del dev {tap} mode tap{multi_queue}",
            tap = net.tap_name,
            multi_queue = multi_queue,
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
/// Sequence: tmpfs /etc -> resolv.conf -> selected netdev up ->
/// volume bind-mounts -> env exports -> init phases.
fn render_init_script(uml: &Umlfile) -> Result<String> {
    let mut s = String::new();
    s.push_str("#!/bin/bash\n");
    s.push_str("# Auto-generated by `umlctl up`. Do not edit by hand -\n");
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
    s.push_str("# resolve sys.executable - breaking subprocess.Popen,\n");
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
    //   /proc       - /proc/self/exe, /proc/cpuinfo, etc.
    //   /sys        - sysfs (cgroups, network info, etc.)
    //   /dev/pts    - devpts, required for os.openpty() and TTY tests
    //   /dev/shm    - POSIX shm_open / multiprocessing.shared_memory
    //   /tmp        - tmpfs, ensures fresh writable scratch
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

    // tmpfs /etc so we can write resolv.conf + hosts without touching
    // the host's /etc (under hostfs that file is the host's).  The
    // mount is private to this UML instance and goes away on shutdown.
    //
    // CRITICAL: the tmpfs shadows the host's /etc/services,
    // /etc/nsswitch.conf, /etc/protocols, etc.  getaddrinfo()
    // consults /etc/services to resolve port names (e.g. "http")
    // and nsswitch.conf to decide whether to consult /etc/hosts at
    // all.  Without them, asyncio + multiprocessing tests that bind
    // to named ports fail with
    //   socket.gaierror: [Errno -8] Servname not supported
    //
    // Stash the essential files in /tmp BEFORE the tmpfs mount (we
    // already mounted tmpfs on /tmp above), then restore them after.
    // hostfs is read-write to /tmp from the guest, so this stash is
    // a guest-side copy not a host-touching one.
    s.push_str("# Stash essential /etc files BEFORE the tmpfs overlay so they\n");
    s.push_str("# survive the mount.  Without these, tests like test_grp,\n");
    s.push_str("# test_pwd, test_socket, test_asyncio.test_subprocess,\n");
    s.push_str("# test___all__ fail with surprising errors (getpwuid,\n");
    s.push_str("# getgrnam, getservbyname, nsswitch resolution, broken\n");
    s.push_str("# /usr/lib/python3.X/sitecustomize.py symlink).\n");
    s.push_str("mkdir -p /tmp/.umlctl-etc-stash 2>/dev/null\n");
    s.push_str("for f in services nsswitch.conf protocols passwd group \\\n");
    s.push_str("         shadow gshadow hosts.allow hosts.deny ssl \\\n");
    s.push_str("         ca-certificates ld.so.conf ld.so.conf.d \\\n");
    s.push_str("         machine-id localtime timezone apt \\\n");
    s.push_str("         python3 python3.13 python3.14; do\n");
    s.push_str(
        "    [ -e \"/etc/$f\" ] && cp -a \"/etc/$f\" \"/tmp/.umlctl-etc-stash/\" 2>/dev/null\n",
    );
    s.push_str("done\n");
    s.push_str("mount -t tmpfs tmpfs /etc 2>/dev/null || true\n");
    s.push_str("# Restore stashed files into the fresh tmpfs.\n");
    s.push_str("for f in services nsswitch.conf protocols passwd group \\\n");
    s.push_str("         shadow gshadow hosts.allow hosts.deny ssl \\\n");
    s.push_str("         ca-certificates ld.so.conf ld.so.conf.d \\\n");
    s.push_str("         machine-id localtime timezone apt \\\n");
    s.push_str("         python3 python3.13 python3.14; do\n");
    s.push_str("    [ -e \"/tmp/.umlctl-etc-stash/$f\" ] && \\\n");
    s.push_str("        cp -a \"/tmp/.umlctl-etc-stash/$f\" \"/etc/\" 2>/dev/null\n");
    s.push_str("done\n");
    if !uml.network.nameservers.is_empty() {
        s.push_str("cat > /etc/resolv.conf <<'__RESOLV__'\n");
        for ns in &uml.network.nameservers {
            s.push_str(&format!("nameserver {ns}\n"));
        }
        s.push_str("__RESOLV__\n");
    }
    // /etc/hosts is REQUIRED for getaddrinfo("localhost") to succeed
    // without an external DNS lookup.  Many Python tests bind sockets
    // to "localhost" (test_asyncio.test_events, test_multiprocessing*,
    // test_concurrent_futures.*) and would otherwise hit
    //   OSError: [Errno -3] Temporary failure in name resolution
    // even when /etc snapshot above succeeded - the host's /etc/hosts
    // doesn't always include the standard localhost entries (some
    // distros leave it minimal).
    s.push_str("cat > /etc/hosts <<'__HOSTS__'\n");
    s.push_str("127.0.0.1   localhost localhost.localdomain\n");
    s.push_str("::1         localhost ip6-localhost ip6-loopback\n");
    s.push_str("ff02::1     ip6-allnodes\n");
    s.push_str("ff02::2     ip6-allrouters\n");
    s.push_str("__HOSTS__\n");
    s.push_str("\n");

    // Network up (tap-specific config - lo already up above).
    if uml.network.mode == "tap" {
        let dev = tap_network_plan(&uml.network, &uml.runtime)?.guest_dev;

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

    // Volumes - bind-mount src -> dst. UML uses hostfs as root, so
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
    // and requires the remount step to actually flip RW->RO.
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

    render_network_metadata_exports(&mut s, uml)?;

    // Init commands run sequentially. The phase command goes through
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
    // Avoid the userspace halt path.  On many hosts /sbin/halt routes
    // through systemctl, which can block in a UML guest where PID 1 is
    // this shell script rather than systemd.  `echo b > /proc/sysrq
    // -trigger` invokes the kernel's sysrq_handle_reboot path, which
    // calls machine_restart() immediately - no userspace cooperation
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
    // failure path their halt hangs and the gate-loop times out - to
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
        // No phases declared - drop into a shell so the user can poke around.
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

fn render_network_metadata_exports(s: &mut String, uml: &Umlfile) -> Result<()> {
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
        ("UMLCTL_NETWORK_QUEUE_SPEC", String::new()),
        ("UMLCTL_NETWORK_QUEUES", "0".to_string()),
        ("UMLCTL_NETWORK_FD", String::new()),
        ("UMLCTL_NETWORK_FD_COUNT", "0".to_string()),
        ("UMLCTL_NETWORK_FAIL_OPEN_AFTER", String::new()),
    ];

    if uml.network.mode == "tap" {
        let plan = tap_network_plan(&uml.network, &uml.runtime)?;
        let inherited_fd = plan
            .inherited_fd
            .map(|fd| fd.to_string())
            .unwrap_or_default();
        let inherited_fd_count = if plan.inherited_fd.is_some() {
            plan.inherited_fd_count.to_string()
        } else {
            "0".to_string()
        };
        let fail_open_after = plan
            .fail_open_after
            .map(|attempt| attempt.to_string())
            .unwrap_or_default();
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
            ("UMLCTL_NETWORK_QUEUE_SPEC", plan.queue_spec),
            ("UMLCTL_NETWORK_QUEUES", plan.queue_count.to_string()),
            ("UMLCTL_NETWORK_FD", inherited_fd),
            ("UMLCTL_NETWORK_FD_COUNT", inherited_fd_count),
            ("UMLCTL_NETWORK_FAIL_OPEN_AFTER", fail_open_after),
        ];
    }

    s.push_str("# umlctl runtime metadata (reserved; describes the generated plan).\n");
    for (key, value) in rows {
        s.push_str(&format!("export {key}={}\n", shell_quote(&value)));
    }
    s.push_str("\n");
    Ok(())
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

/// Parse "10.7.0.2/24" -> "10.7.0.0/24". Used for the MASQUERADE
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

/// Strip the prefix from "10.7.0.2/24" -> "10.7.0.2".
fn guest_ip_addr(guest_ip: &str) -> Result<String> {
    Ok(guest_ip
        .split('/')
        .next()
        .ok_or_else(|| anyhow!("empty guest_ip"))?
        .to_string())
}

/// Detect the host's default-route iface (`ip -o route show default`).
/// Read the first BogoMIPS line out of /proc/cpuinfo and convert it
/// to a loops_per_jiffy estimate at HZ=1000.  Returns None if the
/// file is unparseable or doesn't have a BogoMIPS line.
///
/// Used by fast-boot to pass `lpj=` on the kernel cmdline so the
/// in-kernel BogoMIPS calibration loop is skipped - that calibration
/// rounds up to the next jiffy and adds ~100 ms of wall-clock jitter.
///
/// The BogoMIPS this reads is from the deploy host, which is the same
/// machine the UML guest will run on.  That keeps udelay()/mdelay()
/// calibration tied to the actual hardware executing the guest.
///
/// The corner that *could* be wrong: if the UML binary is later
/// migrated to a different host without re-running umlctl deploy,
/// the cmdline-baked lpj is stale.  In practice UML's udelay()
/// loops go through the host scheduler anyway, so the calibration
/// error only affects the lower bound of the delay (the host
/// scheduler-imposed upper bound dominates).  The practical
/// downside is small but the corner exists.
///
/// Mitigation if it ever matters: bracket the value with the
/// observed mean across `/proc/cpuinfo` (multi-socket / asymmetric
/// hosts can have varying BogoMIPS per CPU), or pass `lpj=` only
/// when /proc/cpuinfo is stable enough.  For now we use the first
/// BogoMIPS line which is enough for the homogeneous single-socket
/// hosts our bench / serverless use cases target.
fn sniff_host_lpj() -> Option<u64> {
    use std::io::BufRead;
    let f = std::fs::File::open("/proc/cpuinfo").ok()?;
    for line in std::io::BufReader::new(f).lines().flatten() {
        if let Some(rest) = line.strip_prefix("bogomips") {
            // "bogomips\t: 9866.44" -> 9866.44
            let v: f64 = rest
                .trim_start_matches(|c: char| !c.is_ascii_digit() && c != '.')
                .split_whitespace()
                .next()?
                .parse()
                .ok()?;
            // loops_per_jiffy ~= BogoMIPS x 1e6 / 2 (the kernel's
            // calibration formula); cast to u64.
            return Some((v * 500_000.0) as u64);
        }
    }
    None
}

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
/// Translate `Umlfile.host_resources` into the manifest's host_env
/// and cgroup_v2 fields. Empty input fields produce no entries and
/// leave the manifest unchanged. The kernel-side knobs read UM_* env
/// vars at UML startup; this function is the declarative-to-runtime
/// bridge.
pub fn apply_host_resources(hr: &HostResourcesSection, m: &mut crate::manifest::Manifest) {
    if !hr.thp.is_empty() {
        m.host_env.insert("UM_THP".into(), hr.thp.clone());
    }
    if let Some(n) = hr.oom_score_adj {
        m.host_env.insert("UM_OOM_SCORE_ADJ".into(), n.to_string());
    }
    if !hr.cpu_affinity.is_empty() {
        m.host_env
            .insert("UM_KVM_V2_CPU_AFFINITY".into(), hr.cpu_affinity.clone());
    }
    /*
     * "off" is the no-op default; everything else translates to
     * UM_KVM_V2_VCPU_AFFINITY for the kernel-side consumer.
     * "auto" computes the per-vCPU mask from @cpu_affinity at
     * boot (kernel side); explicit lists are passed through
     * verbatim.
     */
    if !hr.vcpu_thread_affinity.is_empty() && hr.vcpu_thread_affinity != "off" {
        m.host_env.insert(
            "UM_KVM_V2_VCPU_AFFINITY".into(),
            hr.vcpu_thread_affinity.clone(),
        );
    }
    if !hr.hugepages.is_empty() && hr.hugepages != "off" {
        m.host_env
            .insert("UM_HUGEPAGES".into(), hr.hugepages.clone());
    }
    if hr.pin_physmem {
        m.host_env
            .insert("UM_KVM_V2_PIN_PHYSMEM".into(), "1".into());
    }

    if !hr.memory_max.is_empty() || !hr.cpu_max.is_empty() || hr.pids_max.is_some() {
        m.cgroup_v2 = Some(crate::manifest::CgroupV2Config {
            memory_max: hr.memory_max.clone(),
            cpu_max: hr.cpu_max.clone(),
            pids_max: hr.pids_max,
        });
    }
}

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

    /// Every HostResourcesSection field that should map to host_env or
    /// cgroup_v2 ends up in the Manifest.
    #[test]
    fn apply_host_resources_populates_host_env_and_cgroup_v2() {
        use std::collections::BTreeMap;
        use std::path::PathBuf;

        let hr = HostResourcesSection {
            thp: "off".to_string(),
            oom_score_adj: Some(500),
            cpu_affinity: "0-3".to_string(),
            vcpu_thread_affinity: "auto".to_string(),
            hugepages: "2M".to_string(),
            pin_physmem: true,
            memory_max: "512M".to_string(),
            cpu_max: "200%".to_string(),
            pids_max: Some(512),
        };

        let mut m = crate::manifest::Manifest {
            schema_version: 1,
            instance: crate::manifest::InstanceSection {
                name: "test".into(),
                created_at: "1970-01-01T00:00:00Z".into(),
            },
            kernel: crate::manifest::KernelSection {
                path: PathBuf::from("/x"),
                sha256: "deadbeef".into(),
                profile: "research".into(),
                backend: "seccomp".into(),
            },
            runtime: crate::manifest::RuntimeSection {
                mem: "256M".into(),
                ncpus: 1,
                cmdline: "".into(),
                root: "hostfs".into(),
                forkserver: false,
            },
            host_env: BTreeMap::new(),
            cgroup_v2: None,
            labels: Default::default(),
        };

        apply_host_resources(&hr, &mut m);

        assert_eq!(m.host_env.get("UM_THP"), Some(&"off".to_string()));
        assert_eq!(m.host_env.get("UM_OOM_SCORE_ADJ"), Some(&"500".to_string()));
        assert_eq!(
            m.host_env.get("UM_KVM_V2_CPU_AFFINITY"),
            Some(&"0-3".to_string())
        );
        assert_eq!(
            m.host_env.get("UM_KVM_V2_VCPU_AFFINITY"),
            Some(&"auto".to_string()),
            "vcpu_thread_affinity plumbed into env"
        );
        assert_eq!(m.host_env.get("UM_HUGEPAGES"), Some(&"2M".to_string()));
        assert_eq!(
            m.host_env.get("UM_KVM_V2_PIN_PHYSMEM"),
            Some(&"1".to_string())
        );

        let cg = m.cgroup_v2.expect("cgroup_v2 populated");
        assert_eq!(cg.memory_max, "512M");
        assert_eq!(cg.cpu_max, "200%");
        assert_eq!(cg.pids_max, Some(512));
    }

    /// And the inverse: an empty / default HostResourcesSection
    /// leaves both fields untouched.  Locks in "you can opt out by
    /// simply not setting [host_resources]".
    #[test]
    fn apply_host_resources_default_leaves_manifest_clean() {
        use std::collections::BTreeMap;
        use std::path::PathBuf;

        let hr = HostResourcesSection::default();
        let mut m = crate::manifest::Manifest {
            schema_version: 1,
            instance: crate::manifest::InstanceSection {
                name: "test".into(),
                created_at: "1970-01-01T00:00:00Z".into(),
            },
            kernel: crate::manifest::KernelSection {
                path: PathBuf::from("/x"),
                sha256: "deadbeef".into(),
                profile: "research".into(),
                backend: "seccomp".into(),
            },
            runtime: crate::manifest::RuntimeSection {
                mem: "256M".into(),
                ncpus: 1,
                cmdline: "".into(),
                root: "hostfs".into(),
                forkserver: false,
            },
            host_env: BTreeMap::new(),
            cgroup_v2: None,
            labels: Default::default(),
        };

        apply_host_resources(&hr, &mut m);

        assert!(
            m.host_env.is_empty(),
            "host_env should stay empty for default HostResourcesSection"
        );
        assert!(
            m.cgroup_v2.is_none(),
            "cgroup_v2 should stay None for default HostResourcesSection"
        );
    }

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
        assert_eq!(u.network.driver, "vector2");
        assert_eq!(u.network.host_mode, "auto");
        assert_eq!(u.network.queues, NetworkQueueSpec::Fixed(1));
        assert_eq!(u.network.fail_open_after, None);
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
        // Explicit driver = "vector" so the test continues to exercise
        // the vec0 render path while the default remains vector2.
        let u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector"
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
        assert!(tap_network_plan(&u.network, &u.runtime)
            .unwrap()
            .kernel_arg
            .starts_with("vec0:"));
        assert!(s.contains("__umlctl_phase 'hello' 'echo hi'"));
    }

    #[test]
    fn vector2_network_driver_defaults_to_fd_handoff() {
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
        assert!(s.contains("export UMLCTL_NETWORK_TRANSPORT='fd'"));
        assert!(s.contains("export UMLCTL_NETWORK_HOST_MODE='fd'"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUE_SPEC='1'"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUES='1'"));
        assert!(s.contains("export UMLCTL_NETWORK_FD='200'"));
        assert!(s.contains("export UMLCTL_NETWORK_FD_COUNT='1'"));
        assert!(s.contains("export UMLCTL_NETWORK_FAIL_OPEN_AFTER=''"));
        let plan = tap_network_plan(&u.network, &u.runtime).unwrap();
        assert_eq!(plan.driver, "vector2");
        assert_eq!(plan.guest_dev, "vec2.0");
        assert_eq!(plan.transport, "fd");
        assert_eq!(plan.host_mode, "fd");
        assert_eq!(plan.queue_spec, "1");
        assert_eq!(plan.queue_count, 1);
        assert_eq!(plan.fail_open_after, None);
        assert_eq!(plan.inherited_fd, Some(VECTOR2_TAP_FD));
        assert_eq!(plan.inherited_fd_count, 1);
        assert_eq!(
            plan.kernel_arg,
            "vec2.0:transport=fd,mode=fd,fd=200,depth=128,gso=1,csum=1",
        );
        let labels = network_plan_labels(&plan);
        assert!(labels.contains(&"umlctl.network.fd=200".to_string()));
        assert!(labels.contains(&"umlctl.network.fd_count=1".to_string()));
        assert!(labels.contains(&"umlctl.network.queue_spec=1".to_string()));
    }

    #[test]
    fn vector2_network_fail_open_after_renders_kernel_arg_and_metadata() {
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
tap_name = "fail-open-tap0"
fail_open_after = 2
"#,
        )
        .unwrap();

        let s = render_init_script(&u).unwrap();
        let plan = tap_network_plan(&u.network, &u.runtime).unwrap();

        assert_eq!(plan.fail_open_after, Some(2));
        assert_eq!(
            plan.kernel_arg,
            "vec2.0:transport=fd,mode=fd,fd=200,depth=128,gso=1,csum=1,fail_open_after=2",
        );
        assert!(s.contains("export UMLCTL_NETWORK_FAIL_OPEN_AFTER='2'"));
        let labels = network_plan_labels(&plan);
        assert!(labels.contains(&"umlctl.network.fail_open_after=2".to_string()));
    }

    #[test]
    fn vector2_network_queues_auto_render_fd_multiqueue_plan() {
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector2"
tap_name = "mq-tap0"
queues = 4
"#,
        )
        .unwrap();
        let tmp = tempfile::tempdir().unwrap();
        u.debug.log_dir = tmp.path().join("logs").display().to_string();

        let s = render_init_script(&u).unwrap();
        let compiled = compile(&u).unwrap();
        let plan = tap_network_plan(&u.network, &u.runtime).unwrap();

        assert!(s.contains("export UMLCTL_NETWORK_QUEUE_SPEC='4'"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUES='4'"));
        assert!(s.contains("export UMLCTL_NETWORK_TRANSPORT='fd'"));
        assert!(s.contains("export UMLCTL_NETWORK_HOST_MODE='fd'"));
        assert!(s.contains("export UMLCTL_NETWORK_FD='200'"));
        assert!(s.contains("export UMLCTL_NETWORK_FD_COUNT='4'"));
        assert!(compiled.setup_steps.iter().any(|step| step
            .starts_with("ip tuntap add dev mq-tap0 mode tap user ")
            && step.ends_with(" multi_queue")));
        assert!(compiled
            .teardown_steps
            .iter()
            .any(|step| step == "ip tuntap del dev mq-tap0 mode tap multi_queue"));
        assert_eq!(plan.queue_count, 4);
        assert_eq!(plan.inherited_fd, Some(VECTOR2_TAP_FD));
        assert_eq!(plan.inherited_fd_count, 4);
        assert_eq!(
            plan.kernel_arg,
            "vec2.0:transport=fd,mode=fd,fd=200,depth=128,gso=1,csum=1,queues=4",
        );
        let labels = network_plan_labels(&plan);
        assert!(labels.contains(&"umlctl.network.fd=200".to_string()));
        assert!(labels.contains(&"umlctl.network.fd_count=4".to_string()));
        assert!(labels.contains(&"umlctl.network.queue_spec=4".to_string()));
    }

    #[test]
    fn vector2_network_auto_queues_resolve_to_runtime_ncpus() {
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[runtime]
ncpus = 4
[network]
mode = "tap"
driver = "vector2"
tap_name = "auto-tap0"
queues = "auto"
"#,
        )
        .unwrap();
        let tmp = tempfile::tempdir().unwrap();
        u.debug.log_dir = tmp.path().join("logs").display().to_string();

        let s = render_init_script(&u).unwrap();
        let compiled = compile(&u).unwrap();
        let plan = tap_network_plan(&u.network, &u.runtime).unwrap();

        assert_eq!(u.network.queues, NetworkQueueSpec::Auto);
        assert_eq!(plan.queue_spec, "auto");
        assert_eq!(plan.queue_count, 4);
        assert_eq!(
            plan.kernel_arg,
            "vec2.0:transport=fd,mode=fd,fd=200,depth=128,gso=1,csum=1,queues=4",
        );
        assert!(s.contains("export UMLCTL_NETWORK_QUEUE_SPEC='auto'"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUES='4'"));
        assert!(toml::to_string(&u).unwrap().contains("queues = \"auto\""));
        assert!(compiled.setup_steps.iter().any(|step| step
            .starts_with("ip tuntap add dev auto-tap0 mode tap user ")
            && step.ends_with(" multi_queue")));
        let labels = network_plan_labels(&plan);
        assert!(labels.contains(&"umlctl.network.queue_spec=auto".to_string()));
        assert!(labels.contains(&"umlctl.network.queues=4".to_string()));
    }

    #[test]
    fn vector2_network_can_force_inproc_single_queue() {
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
host_mode = "inproc"
tap_name = "compat-tap0"
"#,
        )
        .unwrap();
        let plan = tap_network_plan(&u.network, &u.runtime).unwrap();

        assert_eq!(plan.transport, "tap");
        assert_eq!(plan.host_mode, "inproc");
        assert_eq!(plan.inherited_fd, None);
        assert_eq!(plan.inherited_fd_count, 0);
        assert_eq!(
            plan.kernel_arg,
            "vec2.0:transport=tap,mode=inproc,ifname=compat-tap0,depth=128",
        );
    }

    #[test]
    fn set_network_driver_validates_mode() {
        // Explicit driver = "vector" so the test has a concrete value
        // to replace while the default remains vector2.
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector"
"#,
        )
        .unwrap();

        set_network_driver(&mut u, "vector2").unwrap();
        assert_eq!(u.network.driver, "vector2");
        // set_network_driver marks the driver as explicit.
        assert!(u.network.driver_explicit);

        u.network.mode = "none".into();
        // mode!=tap rejects an explicitly-set driver; users must
        // either set mode=tap or drop the explicit driver= line.
        assert!(set_network_driver(&mut u, "vector2").is_err());
        assert!(set_network_driver(&mut u, "bogus").is_err());
    }

    #[test]
    fn set_network_fixed_queues_requires_vector2_tap() {
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector"
"#,
        )
        .unwrap();

        assert!(set_network_queue_spec(&mut u, NetworkQueueSpec::Fixed(0)).is_err());
        assert!(set_network_queue_spec(&mut u, NetworkQueueSpec::Fixed(2)).is_err());
        set_network_driver(&mut u, "vector2").unwrap();
        set_network_queue_spec(&mut u, NetworkQueueSpec::Fixed(2)).unwrap();
        assert_eq!(u.network.queues, NetworkQueueSpec::Fixed(2));
        assert!(set_network_driver(&mut u, "vector").is_err());

        u.network.mode = "none".into();
        assert!(set_network_queue_spec(&mut u, NetworkQueueSpec::Fixed(2)).is_err());
    }

    #[test]
    fn set_network_queue_spec_accepts_auto_only_for_vector2_tap() {
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[runtime]
ncpus = 3
[network]
mode = "tap"
driver = "vector"
"#,
        )
        .unwrap();

        assert!(set_network_queue_spec(&mut u, NetworkQueueSpec::Auto).is_err());
        set_network_driver(&mut u, "vector2").unwrap();
        set_network_queue_spec(&mut u, NetworkQueueSpec::Auto).unwrap();
        assert_eq!(u.network.queues, NetworkQueueSpec::Auto);
        assert_eq!(resolve_network_queues(&u.network, &u.runtime).unwrap(), 3);
        assert!(set_network_driver(&mut u, "vector").is_err());

        u.network.mode = "none".into();
        assert!(set_network_queue_spec(&mut u, NetworkQueueSpec::Auto).is_err());
    }

    #[test]
    fn set_network_host_mode_validates_driver_and_queues() {
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector"
"#,
        )
        .unwrap();

        assert!(set_network_host_mode(&mut u, "fd").is_err());
        set_network_driver(&mut u, "vector2").unwrap();
        set_network_host_mode(&mut u, "fd").unwrap();
        assert_eq!(u.network.host_mode, "fd");
        set_network_queue_spec(&mut u, NetworkQueueSpec::Fixed(2)).unwrap();
        assert_eq!(u.network.queues, NetworkQueueSpec::Fixed(2));
        set_network_host_mode(&mut u, "inproc").unwrap();
        assert_eq!(u.network.host_mode, "inproc");
        set_network_host_mode(&mut u, "fd").unwrap();
        assert_eq!(u.network.host_mode, "fd");
        assert!(set_network_host_mode(&mut u, "bogus").is_err());
    }

    #[test]
    fn network_fail_open_after_requires_vector2_and_positive_value() {
        // Start at the "vector" driver so the test exercises the
        // "fail_open_after requires vector2" path.  Without the
        // explicit driver line this assertion would pass trivially.
        let mut u: Umlfile = toml::from_str(
            r#"
schema_version = 1
[instance]
name = "demo"
[kernel]
path = "/x"
[network]
mode = "tap"
driver = "vector"
"#,
        )
        .unwrap();

        u.network.fail_open_after = Some(1);
        assert!(validate_network_section(&u.network, &u.runtime).is_err());

        u.network.driver = "vector2".into();
        assert!(validate_network_section(&u.network, &u.runtime).is_ok());

        u.network.fail_open_after = Some(0);
        assert!(validate_network_section(&u.network, &u.runtime).is_err());
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

        // mode!=tap rejects an explicitly-set driver. The check uses
        // driver_explicit, which the manual Deserialize impl sets to
        // true whenever `driver` is present in the TOML source.
        u.network.driver = "vector".into();
        u.network.driver_explicit = true;
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
        assert!(s.contains("export UMLCTL_NETWORK_QUEUE_SPEC=''"));
        assert!(s.contains("export UMLCTL_NETWORK_QUEUES='0'"));
        assert!(s.contains("export UMLCTL_NETWORK_FD=''"));
        assert!(s.contains("export UMLCTL_NETWORK_FD_COUNT='0'"));
        assert!(s.contains("export UMLCTL_NETWORK_FAIL_OPEN_AFTER=''"));
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
        // PATH default - required for CPython sys.executable resolution.
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
