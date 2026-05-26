// SPDX-License-Identifier: GPL-2.0
//
// umlbuild profile — TOML schema, loader, and `profile {list|show}` verbs.
//
// A profile is a single TOML describing how to construct a UML instance:
// kernel Kconfig overlay + rootfs base + package set + image size + the
// defaults to bake into the emitted Umlfile.toml. See SPEC.md for the
// authoritative schema.

use anyhow::{anyhow, bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::paths;

/// Top-level profile document. `schema_version = 1` at v1.
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub schema_version: u32,
    pub profile: ProfileMeta,
    pub kernel: KernelSpec,
    pub rootfs: RootfsSpec,
    pub image: ImageSpec,
    #[serde(default)]
    pub instance: InstanceDefaults,
    #[serde(default)]
    pub network: NetworkSpec,
}

/// Networking knobs baked into the rootfs at build time + carried
/// through to the kernel cmdline at boot.
///
/// `mode = "none"` (default) produces a guest with only loopback.
/// `mode = "tap"` requires the operator to bring up the host TAP
/// (umlbuild shell --network tap handles this via sudo); the guest's
/// /sbin/init brings up the corresponding NIC inside the guest and
/// writes /etc/resolv.conf from `dns`.
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields, default)]
pub struct NetworkSpec {
    pub mode: String,          // "none" or "tap"
    pub driver: String,        // "vector" (v1) or "vector2"
    pub tap_name: String,      // host-side TAP device name
    pub host_ip: String,       // e.g. "10.7.0.1/24"
    pub guest_ip: String,      // e.g. "10.7.0.2/24"
    pub gateway: String,       // e.g. "10.7.0.1"
    pub dns: Vec<String>,      // e.g. ["1.1.1.1", "8.8.8.8"]
}

impl Default for NetworkSpec {
    fn default() -> Self {
        Self {
            mode: "none".into(),
            // v2 default: current architecture, no per-packet GSO
            // log spam, `vec2.0` guest interface name.  Set
            // driver = "vector" explicitly to use the legacy v1
            // driver.
            driver: "vector2".into(),
            tap_name: "umlb-tap0".into(),
            host_ip: "10.7.0.1/24".into(),
            guest_ip: "10.7.0.2/24".into(),
            gateway: "10.7.0.1".into(),
            dns: vec!["1.1.1.1".into(), "8.8.8.8".into()],
        }
    }
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct ProfileMeta {
    pub name: String,
    #[serde(default)]
    pub description: String,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct KernelSpec {
    /// One of: "tinyconfig", "base_defconfig", "x86_64_defconfig",
    /// or an explicit defconfig path relative to the kernel source root.
    pub base_config: String,
    #[serde(default)]
    pub enable: Vec<String>,
    #[serde(default)]
    pub disable: Vec<String>,
    /// Additional values like `CONFIG_LOG_BUF_SHIFT=14`.  Optional; rare.
    #[serde(default)]
    pub set: BTreeMap<String, String>,
    #[serde(default)]
    pub strip: bool,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct RootfsSpec {
    /// "alpine" or "debian-slim" at v1.
    pub base: String,
    #[serde(default)]
    pub alpine: Option<AlpineSpec>,
    #[serde(default)]
    pub debian: Option<DebianSpec>,
    /// uid the sandbox script will drop to inside the guest.  0 means
    /// "stay as root inside the rootfs".
    #[serde(default = "default_sandbox_uid")]
    pub sandbox_uid: u32,
}

fn default_sandbox_uid() -> u32 {
    1000
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct AlpineSpec {
    pub version: String,
    pub sha256: String,
    #[serde(default = "default_alpine_mirror")]
    pub mirror: String,
    #[serde(default)]
    pub packages: Vec<String>,
}

fn default_alpine_mirror() -> String {
    "https://dl-cdn.alpinelinux.org/alpine".into()
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct DebianSpec {
    #[serde(default = "default_debian_suite")]
    pub suite: String,
    #[serde(default = "default_debian_variant")]
    pub variant: String,
    #[serde(default)]
    pub packages: Vec<String>,
}

fn default_debian_suite() -> String {
    "bookworm".into()
}
fn default_debian_variant() -> String {
    "minbase".into()
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct ImageSpec {
    /// Size string accepted by truncate(1): "200M", "2G".
    pub size: String,
    #[serde(default = "default_fstype")]
    pub fstype: String,
    #[serde(default = "default_label")]
    pub label: String,
}

fn default_fstype() -> String {
    "ext4".into()
}
fn default_label() -> String {
    "uml-sandbox".into()
}

#[derive(Serialize, Deserialize, Debug, Clone, Default)]
#[serde(deny_unknown_fields)]
pub struct InstanceDefaults {
    #[serde(default = "default_mem")]
    pub mem: String,
    #[serde(default = "default_ncpus")]
    pub ncpus: u32,
    #[serde(default = "default_backend")]
    pub backend: String,
    /// Default sandbox command baked into the emitted Umlfile.
    #[serde(default)]
    pub sandbox_cmd: String,
}

fn default_mem() -> String {
    "256M".into()
}
fn default_ncpus() -> u32 {
    1
}
fn default_backend() -> String {
    "seccomp".into()
}

// ----------------------------------------------------------------------
// Resolution: take a profile NAME or PATH, return a parsed Profile.
// ----------------------------------------------------------------------

/// Resolve a profile reference to a parsed Profile.  The reference is
/// either an explicit path (contains '/' or ends with .toml) or a bare
/// profile name searched against the user config dir then the in-tree
/// profiles dir.
pub fn resolve(reference: &str) -> Result<Profile> {
    let p = paths::Paths::resolve()?;
    let candidate_path = if reference.contains('/') || reference.ends_with(".toml") {
        PathBuf::from(reference)
    } else {
        let user = p.user_profile_dir().join(format!("{reference}.toml"));
        if user.is_file() {
            user
        } else {
            // Fall back to in-tree.  Locate kernel source from CWD.
            let cwd = std::env::current_dir().context("getcwd")?;
            let src = paths::find_kernel_source(&cwd).ok_or_else(|| {
                anyhow!(
                    "profile '{reference}' not found in {user} and could not \
                     locate kernel source from {cwd} to fall back to in-tree \
                     profiles",
                    user = user.display(),
                    cwd = cwd.display(),
                )
            })?;
            paths::in_tree_profile_dir(&src).join(format!("{reference}.toml"))
        }
    };
    load(&candidate_path)
}

pub fn load(path: &Path) -> Result<Profile> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("read profile {}", path.display()))?;
    let prof: Profile = toml::from_str(&text)
        .with_context(|| format!("parse profile TOML {}", path.display()))?;
    validate(&prof)?;
    Ok(prof)
}

fn validate(p: &Profile) -> Result<()> {
    if p.schema_version != 1 {
        bail!(
            "unsupported schema_version {}; this umlbuild supports 1",
            p.schema_version
        );
    }
    match p.rootfs.base.as_str() {
        "alpine" => {
            if p.rootfs.alpine.is_none() {
                bail!("rootfs.base = \"alpine\" requires a [rootfs.alpine] section");
            }
        }
        "debian-slim" => {
            // debian section is optional (defaults are fine).
        }
        other => bail!("unsupported rootfs.base = \"{other}\" (use alpine or debian-slim)"),
    }
    if p.profile.name.is_empty() {
        bail!("profile.name must be non-empty");
    }
    if !p
        .profile
        .name
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
    {
        bail!(
            "profile.name '{}' must match [a-zA-Z0-9_-]+",
            p.profile.name
        );
    }
    Ok(())
}

// ----------------------------------------------------------------------
// Subcommand dispatch.
// ----------------------------------------------------------------------

#[derive(clap::Subcommand, Debug)]
pub enum ProfileCmd {
    /// List built-in and user profiles.
    List,
    /// Print the resolved profile contents.
    Show {
        /// Profile name or path to a profile TOML.
        name: String,
    },
}

pub fn run(cmd: ProfileCmd) -> Result<()> {
    match cmd {
        ProfileCmd::List => cmd_list(),
        ProfileCmd::Show { name } => cmd_show(&name),
    }
}

fn cmd_list() -> Result<()> {
    let p = paths::Paths::resolve()?;
    let mut found: Vec<(String, PathBuf, &'static str)> = Vec::new();

    // In-tree profiles.
    if let Some(src) = paths::find_kernel_source(&std::env::current_dir()?) {
        let dir = paths::in_tree_profile_dir(&src);
        if let Ok(rd) = std::fs::read_dir(&dir) {
            for entry in rd.flatten() {
                let pp = entry.path();
                if pp.extension().and_then(|s| s.to_str()) == Some("toml") {
                    if let Some(stem) = pp.file_stem().and_then(|s| s.to_str()) {
                        found.push((stem.to_string(), pp, "in-tree"));
                    }
                }
            }
        }
    }

    // User-config profiles.
    if let Ok(rd) = std::fs::read_dir(p.user_profile_dir()) {
        for entry in rd.flatten() {
            let pp = entry.path();
            if pp.extension().and_then(|s| s.to_str()) == Some("toml") {
                if let Some(stem) = pp.file_stem().and_then(|s| s.to_str()) {
                    found.push((stem.to_string(), pp, "user"));
                }
            }
        }
    }

    if found.is_empty() {
        println!("(no profiles found)");
        println!();
        println!("Looked under:");
        println!("  {}", p.user_profile_dir().display());
        println!("  <kernel-source>/tools/uml/uml-launcher/profiles/");
        return Ok(());
    }

    found.sort_by(|a, b| a.0.cmp(&b.0));
    println!("{:<24} {:<8} PATH", "NAME", "SOURCE");
    for (name, path, src) in found {
        println!("{:<24} {:<8} {}", name, src, path.display());
    }
    Ok(())
}

fn cmd_show(name: &str) -> Result<()> {
    let prof = resolve(name)?;
    let pretty = toml::to_string_pretty(&prof).context("re-emit profile as TOML")?;
    print!("{pretty}");
    Ok(())
}
