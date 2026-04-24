// SPDX-License-Identifier: GPL-2.0
//
// TOML manifest schema for umlctl instances.
//
// Immutable after `create` — mutations are `rm` + `create`, which
// prevents the "manifest drifted under a running instance" class
// of bug. `schema_version = 1` is the v1 wire format; a future
// version bump requires a `umlctl migrate` pass (not in v1).

use anyhow::{anyhow, bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct Manifest {
    pub schema_version: u32,
    pub instance: InstanceSection,
    pub kernel: KernelSection,
    pub runtime: RuntimeSection,
    #[serde(default)]
    pub labels: BTreeMap<String, String>,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct InstanceSection {
    pub name: String,
    pub created_at: String,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct KernelSection {
    pub path: PathBuf,
    pub sha256: String,
    pub profile: String,
    pub backend: String,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct RuntimeSection {
    pub mem: String,
    pub ncpus: u32,
    pub cmdline: String,
    pub root: String,
    pub forkserver: bool,
}

/// Validate an instance name against `^[a-z0-9][a-z0-9_.-]{0,63}$`.
/// Podman-style — rejected early at `create` time, never again.
pub fn validate_name(name: &str) -> Result<()> {
    if name.is_empty() {
        bail!("instance name cannot be empty");
    }
    if name.len() > 64 {
        bail!("instance name must be 1-64 chars (got {})", name.len());
    }
    let mut chars = name.chars();
    let first = chars.next().unwrap();
    if !(first.is_ascii_lowercase() || first.is_ascii_digit()) {
        bail!("instance name must start with [a-z0-9] (got {first:?})");
    }
    for c in chars {
        let ok = c.is_ascii_lowercase()
            || c.is_ascii_digit()
            || c == '_'
            || c == '.'
            || c == '-';
        if !ok {
            bail!("instance name has invalid character {c:?} (allowed: [a-z0-9_.-])");
        }
    }
    Ok(())
}

impl Manifest {
    #[allow(clippy::too_many_arguments)]
    pub fn from_create_args(
        name: &str,
        kernel: &Path,
        profile: Option<&str>,
        mem: &str,
        backend: &str,
        cmdline: &str,
        root: &str,
        forkserver: bool,
        ncpus: u32,
        labels: &[String],
    ) -> Result<Self> {
        let kernel_canon = kernel
            .canonicalize()
            .with_context(|| format!("canonicalize kernel path {}", kernel.display()))?;
        let sha256 = hash_file(&kernel_canon)
            .with_context(|| format!("hash kernel {}", kernel_canon.display()))?;
        let created_at = now_rfc3339();
        let labels_map = parse_labels(labels)?;

        Ok(Self {
            schema_version: 1,
            instance: InstanceSection {
                name: name.to_string(),
                created_at,
            },
            kernel: KernelSection {
                path: kernel_canon,
                sha256,
                profile: profile.unwrap_or("auto").to_string(),
                backend: backend.to_string(),
            },
            runtime: RuntimeSection {
                mem: mem.to_string(),
                ncpus,
                cmdline: cmdline.to_string(),
                root: root.to_string(),
                forkserver,
            },
            labels: labels_map,
        })
    }

    pub fn read(path: &Path) -> Result<Self> {
        let s = std::fs::read_to_string(path)
            .with_context(|| format!("read {}", path.display()))?;
        let m: Manifest = toml::from_str(&s)
            .with_context(|| format!("parse manifest {}", path.display()))?;
        if m.schema_version != 1 {
            bail!(
                "manifest {} has schema_version {} (this umlctl only handles v1)",
                path.display(),
                m.schema_version
            );
        }
        Ok(m)
    }

    pub fn write_to(&self, path: &Path) -> Result<()> {
        let s = toml::to_string_pretty(self).context("serialize manifest")?;
        std::fs::write(path, s)
            .with_context(|| format!("write {}", path.display()))?;
        Ok(())
    }
}

fn hash_file(path: &Path) -> Result<String> {
    use sha2::{Digest, Sha256};
    let mut f = std::fs::File::open(path)?;
    let mut hasher = Sha256::new();
    std::io::copy(&mut f, &mut hasher)?;
    Ok(hex::encode(hasher.finalize()))
}

fn parse_labels(labels: &[String]) -> Result<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    for entry in labels {
        let (k, v) = entry
            .split_once('=')
            .ok_or_else(|| anyhow!("label must be K=V, got {entry:?}"))?;
        out.insert(k.to_string(), v.to_string());
    }
    Ok(out)
}

/// RFC3339 timestamp with local offset (UTC fallback if the
/// offset is indeterminate — e.g. inside a container without
/// /etc/localtime).
pub(super) fn now_rfc3339() -> String {
    use time::{format_description::well_known::Rfc3339, OffsetDateTime, UtcOffset};
    let now = OffsetDateTime::now_utc();
    let local = UtcOffset::current_local_offset().unwrap_or(UtcOffset::UTC);
    now.to_offset(local)
        .format(&Rfc3339)
        .unwrap_or_else(|_| now.format(&Rfc3339).unwrap_or_default())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn name_validation_matches_spec() {
        assert!(validate_name("foo").is_ok());
        assert!(validate_name("foo-1").is_ok());
        assert!(validate_name("foo_bar.baz").is_ok());
        assert!(validate_name("0abc").is_ok());

        assert!(validate_name("").is_err());
        assert!(validate_name("-foo").is_err());
        assert!(validate_name(".foo").is_err());
        assert!(validate_name("Foo").is_err());
        assert!(validate_name("foo bar").is_err());
        assert!(validate_name("foo/bar").is_err());
        assert!(validate_name(&"a".repeat(65)).is_err());
    }

    #[test]
    fn labels_parse() {
        let m = parse_labels(&["env=prod".into(), "team=uml".into()]).unwrap();
        assert_eq!(m.get("env").map(String::as_str), Some("prod"));
        assert_eq!(m.get("team").map(String::as_str), Some("uml"));
    }

    #[test]
    fn bad_label_rejected() {
        assert!(parse_labels(&["no-equals".into()]).is_err());
    }
}
