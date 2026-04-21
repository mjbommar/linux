// SPDX-License-Identifier: GPL-2.0
//
// Config loader. Merges CLI > env > TOML > defaults via figment.
// The result is a flattened Config struct that launcher.rs
// consumes — keeping the TOML/env/CLI mapping local here so the
// rest of the binary only sees the resolved values.

use std::path::PathBuf;

use anyhow::{anyhow, Context, Result};
use figment::{
    providers::{Format, Serialized, Toml},
    Figment,
};
use serde::{Deserialize, Serialize};

use crate::cli::{Console, RunArgs};

/// Fully-resolved launcher configuration, post-merge.
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Config {
    pub kernel: PathBuf,
    pub init: PathBuf,
    pub mem: String,
    pub root: String,
    pub console: Console,
    pub forkserver: Option<(i32, i32)>,
    pub append: Vec<String>,
    pub dry_run: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            // No sensible default for kernel; load() rejects if unset.
            kernel: PathBuf::new(),
            init: PathBuf::from("/bin/sh"),
            mem: "128M".to_string(),
            root: "hostfs".to_string(),
            console: Console::Stdio,
            forkserver: None,
            append: Vec::new(),
            dry_run: false,
        }
    }
}

/// CLI-provided overlay: only fields the user set. figment merges
/// this as the highest-priority provider.
///
/// Every CLI field is Option<_> and `skip_serializing_if =
/// "Option::is_none"`: if the user did not pass the flag, the
/// overlay contributes nothing and the lower-priority layers
/// (env, TOML, Config::default) decide the value. This is what
/// keeps CLI > env > file > defaults actually working. If a field
/// were passed as a bare String with a clap default_value, clap
/// would always populate it, the overlay would always serialize
/// it, and the TOML/env layers would never be visible to the
/// merged output.
#[derive(Serialize, Default)]
struct CliOverlay {
    #[serde(skip_serializing_if = "Option::is_none")]
    kernel: Option<PathBuf>,
    #[serde(skip_serializing_if = "Option::is_none")]
    init: Option<PathBuf>,
    #[serde(skip_serializing_if = "Option::is_none")]
    mem: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    root: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    console: Option<Console>,
    #[serde(skip_serializing_if = "Option::is_none")]
    forkserver: Option<(i32, i32)>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    append: Vec<String>,
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    dry_run: bool,
}

impl From<&RunArgs> for CliOverlay {
    fn from(args: &RunArgs) -> Self {
        Self {
            kernel: args.kernel.clone(),
            init: args.init.clone(),
            mem: args.mem.clone(),
            root: args.root.clone(),
            console: args.console,
            forkserver: args.forkserver,
            append: args.append.clone(),
            dry_run: args.dry_run,
        }
    }
}

/// Load the effective config from all sources.
///
/// Precedence (highest first): CLI > env (UML_*) > TOML file >
/// built-in defaults. Uses figment's provider model.
pub fn load(args: &RunArgs) -> Result<Config> {
    let mut fig = Figment::from(Serialized::defaults(Config::default()));

    if let Some(ref path) = args.config {
        fig = fig.merge(Toml::file(path));
    }

    // Env-var overlay under UML_* prefix. clap already handled
    // env = "UML_*" for its own struct; figment pulls any we
    // didn't wire explicitly (future proofing for config-only
    // knobs).
    fig = fig.merge(figment::providers::Env::prefixed("UML_").split("__"));

    fig = fig.merge(Serialized::defaults(CliOverlay::from(args)));

    let cfg: Config = fig
        .extract()
        .context("merging launcher config (cli > env > file > defaults)")?;

    if cfg.kernel.as_os_str().is_empty() {
        return Err(anyhow!(
            "no kernel specified (try --kernel <path> or UML_KERNEL=<path> or `kernel = \"...\"` in --config <file>)"
        ));
    }

    Ok(cfg)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run_args_with_kernel(k: &str) -> RunArgs {
        RunArgs {
            kernel: Some(PathBuf::from(k)),
            init: None,
            mem: None,
            root: None,
            console: None,
            forkserver: None,
            append: Vec::new(),
            config: None,
            dry_run: false,
        }
    }

    #[test]
    fn defaults_apply() {
        let cfg = load(&run_args_with_kernel("/tmp/linux")).unwrap();
        assert_eq!(cfg.kernel, PathBuf::from("/tmp/linux"));
        assert_eq!(cfg.init, PathBuf::from("/bin/sh"));
        assert_eq!(cfg.mem, "128M");
        assert_eq!(cfg.root, "hostfs");
    }

    #[test]
    fn cli_overrides_defaults() {
        let mut args = run_args_with_kernel("/tmp/linux");
        args.mem = Some("512M".to_string());
        args.init = Some(PathBuf::from("/bin/true"));
        let cfg = load(&args).unwrap();
        assert_eq!(cfg.mem, "512M");
        assert_eq!(cfg.init, PathBuf::from("/bin/true"));
    }

    #[test]
    fn missing_kernel_errors() {
        let mut args = run_args_with_kernel("");
        args.kernel = None;
        let err = load(&args).unwrap_err();
        assert!(err.to_string().contains("no kernel specified"));
    }

    #[test]
    fn forkserver_passes_through() {
        let mut args = run_args_with_kernel("/tmp/linux");
        args.forkserver = Some((198, 199));
        let cfg = load(&args).unwrap();
        assert_eq!(cfg.forkserver, Some((198, 199)));
    }

    #[test]
    fn append_passes_through() {
        let mut args = run_args_with_kernel("/tmp/linux");
        args.append = vec!["quiet".to_string(), "foo=bar".to_string()];
        let cfg = load(&args).unwrap();
        assert_eq!(cfg.append, vec!["quiet", "foo=bar"]);
    }

    /// Regression: root and console default through the CLI layer
    /// without silently clobbering lower-priority overlays. When
    /// the user omits `--root` / `--console`, the CliOverlay must
    /// serialize them as absent so a TOML file's `root =
    /// "/dev/ubda"` or `console = "null"` is visible to the
    /// merged Config. Previously these were bare String/enum with
    /// clap defaults, which defeated the precedence chain.
    #[test]
    fn toml_overrides_apply_when_cli_absent() {
        use std::io::Write;
        let mut f = tempfile::NamedTempFile::new().unwrap();
        writeln!(
            f,
            "kernel = \"/tmp/linux\"\nroot = \"/dev/ubda\"\nconsole = \"null\"\n"
        )
        .unwrap();

        let mut args = run_args_with_kernel("");
        args.kernel = None; // let TOML provide kernel too
        args.config = Some(f.path().to_path_buf());

        let cfg = load(&args).unwrap();
        assert_eq!(cfg.root, "/dev/ubda");
        assert!(matches!(cfg.console, Console::Null));
    }

    #[test]
    fn cli_overrides_toml_for_root_and_console() {
        use std::io::Write;
        let mut f = tempfile::NamedTempFile::new().unwrap();
        writeln!(
            f,
            "kernel = \"/tmp/linux\"\nroot = \"/dev/ubda\"\nconsole = \"null\"\n"
        )
        .unwrap();

        let mut args = run_args_with_kernel("");
        args.kernel = None;
        args.config = Some(f.path().to_path_buf());
        args.root = Some("hostfs".to_string());
        args.console = Some(Console::Stdio);

        let cfg = load(&args).unwrap();
        assert_eq!(cfg.root, "hostfs");
        assert!(matches!(cfg.console, Console::Stdio));
    }
}
