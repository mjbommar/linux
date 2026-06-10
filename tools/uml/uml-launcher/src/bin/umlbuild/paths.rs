// SPDX-License-Identifier: GPL-2.0
//
// umlbuild — XDG-rooted cache and config layout.
//
// Mirrors umlctl's `paths.rs` shape but for build-side state:
//
//   $XDG_CACHE_HOME/uml-build/        (default ~/.cache/uml-build)
//     dl/        downloaded base tarballs (sha-verified, content-addressed)
//     builds/    kernel O= trees, one per profile
//     rootfs/    staged rootfs directories, one per profile
//     images/    finished ubd images
//
//   $XDG_CONFIG_HOME/uml-build/       (default ~/.config/uml-build)
//     profiles/  user-provided profile TOMLs
//
// In-tree profiles ship at tools/uml/uml-launcher/profiles/; resolution
// order is: explicit path > user config dir > in-tree.

use anyhow::{Context, Result};
use std::path::{Path, PathBuf};

/// Resolved cache + config paths for the running user.
#[derive(Debug, Clone)]
pub struct Paths {
    pub cache_root: PathBuf,
    pub config_root: PathBuf,
}

impl Paths {
    /// Resolve from environment (XDG_CACHE_HOME / XDG_CONFIG_HOME, falling
    /// back to ~/.cache and ~/.config). Creates directories eagerly so
    /// callers can assume they exist.
    pub fn resolve() -> Result<Self> {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .context("HOME is not set; cannot resolve XDG fallback")?;

        let cache_root = std::env::var_os("XDG_CACHE_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".cache"))
            .join("uml-build");

        let config_root = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".config"))
            .join("uml-build");

        for sub in ["dl", "builds", "rootfs", "images"] {
            std::fs::create_dir_all(cache_root.join(sub))
                .with_context(|| format!("create cache subdir {sub}"))?;
        }
        std::fs::create_dir_all(config_root.join("profiles"))
            .context("create config/profiles dir")?;

        Ok(Self {
            cache_root,
            config_root,
        })
    }

    pub fn dl_dir(&self) -> PathBuf {
        self.cache_root.join("dl")
    }

    pub fn build_dir(&self, profile: &str) -> PathBuf {
        self.cache_root.join("builds").join(profile)
    }

    pub fn rootfs_dir(&self, profile: &str) -> PathBuf {
        self.cache_root.join("rootfs").join(profile)
    }

    #[allow(dead_code)] // reserved for image-producing build flows
    pub fn image_path(&self, profile: &str) -> PathBuf {
        self.cache_root
            .join("images")
            .join(format!("{profile}.img"))
    }

    pub fn user_profile_dir(&self) -> PathBuf {
        self.config_root.join("profiles")
    }
}

/// Walk up from `start` looking for a kernel source tree (identified by
/// the presence of `arch/um/Kconfig`). Returns the first match or None.
pub fn find_kernel_source(start: &Path) -> Option<PathBuf> {
    let mut cur: Option<&Path> = Some(start);
    while let Some(p) = cur {
        if p.join("arch/um/Kconfig").is_file() {
            return Some(p.to_path_buf());
        }
        cur = p.parent();
    }
    None
}

/// Resolve the in-tree profile directory relative to a kernel source root.
pub fn in_tree_profile_dir(source_root: &Path) -> PathBuf {
    source_root.join("tools/uml/uml-launcher/profiles")
}
