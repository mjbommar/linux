// SPDX-License-Identifier: GPL-2.0
//
// umlbuild kernel — build a UML kernel binary from a profile's Kconfig
// overlay. See SPEC.md §"umlbuild kernel" for the full contract.

use anyhow::{bail, Context, Result};
use clap::Args;
use std::path::PathBuf;
use std::process::Command;

use crate::{paths, profile};

#[derive(Args, Debug)]
pub struct KernelArgs {
    /// Profile name or path to a profile TOML.
    #[arg(long, value_name = "NAME_OR_PATH")]
    pub profile: String,

    /// Kernel source root.  Defaults to walking up from CWD until
    /// arch/um/Kconfig is found.
    #[arg(long, value_name = "PATH")]
    pub source: Option<PathBuf>,

    /// Output path for the built kernel binary.  Defaults to
    /// $XDG_CACHE_HOME/uml-build/builds/<profile>/linux.
    #[arg(long, value_name = "PATH")]
    pub out: Option<PathBuf>,

    /// Parallelism for `make -j`.  Defaults to the host CPU count.
    #[arg(long, short = 'j', value_name = "N")]
    pub jobs: Option<usize>,

    /// Force rebuild even when the cached config matches.
    #[arg(long)]
    pub force: bool,
}

pub fn run(args: KernelArgs) -> Result<()> {
    let prof = profile::resolve(&args.profile)?;
    let p = paths::Paths::resolve()?;

    let source = match args.source {
        Some(s) => s,
        None => paths::find_kernel_source(&std::env::current_dir()?).ok_or_else(|| {
            anyhow::anyhow!(
                "could not locate kernel source (no arch/um/Kconfig walking \
                 up from CWD); pass --source explicitly"
            )
        })?,
    };

    if !source.join("arch/um/Kconfig").is_file() {
        bail!(
            "{} does not look like a kernel source tree (no arch/um/Kconfig)",
            source.display()
        );
    }

    let build_dir = p.build_dir(&prof.profile.name);
    std::fs::create_dir_all(&build_dir)
        .with_context(|| format!("create build dir {}", build_dir.display()))?;

    let jobs = args
        .jobs
        .unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2));

    tracing::info!(
        profile = %prof.profile.name,
        source = %source.display(),
        build_dir = %build_dir.display(),
        jobs,
        "umlbuild kernel: starting"
    );

    // Phase 1: write the base .config.
    write_base_config(&source, &build_dir, &prof.kernel.base_config)?;

    // Phase 2: apply the Kconfig overlay.
    apply_overlay(&source, &build_dir, &prof.kernel)?;

    // Phase 3: olddefconfig + make.
    let config_sha = sha256_of_file(&build_dir.join(".config"))?;
    let stamp = build_dir.join(".umlbuild-config-sha256");
    let prior = std::fs::read_to_string(&stamp).ok();
    let need_build = args.force || prior.as_deref() != Some(&config_sha);

    if !need_build {
        tracing::info!(
            "umlbuild kernel: cached config matches (sha256={config_sha}); \
             skipping make. Pass --force to rebuild."
        );
    } else {
        run_make(
            &source,
            &build_dir,
            jobs,
            &["ARCH=um", "olddefconfig"],
            "olddefconfig",
        )?;
        run_make(&source, &build_dir, jobs, &["ARCH=um"], "build")?;
        std::fs::write(&stamp, &config_sha)
            .with_context(|| format!("write stamp {}", stamp.display()))?;
    }

    // Phase 4: optional strip, copy to --out.
    let built = build_dir.join("linux");
    if !built.is_file() {
        bail!("expected {} to exist after make", built.display());
    }
    let out = args
        .out
        .unwrap_or_else(|| build_dir.join("linux"));
    if out != built {
        if let Some(parent) = out.parent() {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("create parent of {}", out.display()))?;
        }
        std::fs::copy(&built, &out)
            .with_context(|| format!("copy {} -> {}", built.display(), out.display()))?;
    }
    if prof.kernel.strip {
        let status = Command::new("strip")
            .arg(&out)
            .status()
            .with_context(|| format!("spawn strip {}", out.display()))?;
        if !status.success() {
            bail!("strip exited with {status}");
        }
    }

    // Phase 5: emit a sidecar manifest.
    let size = std::fs::metadata(&out)
        .with_context(|| format!("stat {}", out.display()))?
        .len();
    let bin_sha = sha256_of_file(&out)?;
    let manifest = build_dir.join("manifest.toml");
    let mtext = format!(
        "schema_version = 1\n\
         [kernel]\n\
         profile      = \"{}\"\n\
         source       = \"{}\"\n\
         output       = \"{}\"\n\
         size_bytes   = {}\n\
         binary_sha256 = \"{}\"\n\
         config_sha256 = \"{}\"\n\
         stripped     = {}\n",
        prof.profile.name,
        source.display(),
        out.display(),
        size,
        bin_sha,
        config_sha,
        prof.kernel.strip,
    );
    std::fs::write(&manifest, mtext)
        .with_context(|| format!("write manifest {}", manifest.display()))?;

    eprintln!(
        "umlbuild kernel: {} ({} bytes, sha256={})",
        out.display(),
        size,
        &bin_sha[..16]
    );
    Ok(())
}

fn write_base_config(
    source: &std::path::Path,
    build_dir: &std::path::Path,
    base: &str,
) -> Result<()> {
    // Resolve the base config target.  Accepts:
    //   - "tinyconfig", "allnoconfig", "allyesconfig", any make target
    //   - "base_defconfig", "x86_64_defconfig" -> defconfig from arch/um/configs/
    //   - explicit path
    let target = if base.contains('/') || base.ends_with("_defconfig") {
        base.to_string()
    } else if base.ends_with("config") {
        // "tinyconfig", "allnoconfig", etc.  Pass through.
        base.to_string()
    } else {
        bail!("unrecognized kernel.base_config: {base}");
    };

    run_make(
        source,
        build_dir,
        1,
        &["ARCH=um", target.as_str()],
        &format!("base_config={target}"),
    )?;
    Ok(())
}

fn apply_overlay(
    source: &std::path::Path,
    build_dir: &std::path::Path,
    k: &profile::KernelSpec,
) -> Result<()> {
    let cfg = build_dir.join(".config");
    let scripts_config = source.join("scripts/config");
    if !scripts_config.is_file() {
        bail!("missing {}", scripts_config.display());
    }
    let mut args: Vec<String> = vec!["--file".into(), cfg.display().to_string()];
    for sym in &k.enable {
        args.push("--enable".into());
        args.push(sym.clone());
    }
    for sym in &k.disable {
        args.push("--disable".into());
        args.push(sym.clone());
    }
    for (key, value) in &k.set {
        args.push("--set-val".into());
        args.push(key.clone());
        args.push(value.clone());
    }
    if k.enable.is_empty() && k.disable.is_empty() && k.set.is_empty() {
        return Ok(());
    }
    let status = Command::new(&scripts_config)
        .args(&args)
        .status()
        .with_context(|| format!("run {}", scripts_config.display()))?;
    if !status.success() {
        bail!("scripts/config exited with {status}");
    }
    Ok(())
}

fn run_make(
    source: &std::path::Path,
    build_dir: &std::path::Path,
    jobs: usize,
    args: &[&str],
    phase: &str,
) -> Result<()> {
    let log_path = build_dir.join(format!("umlbuild-{phase}.log"));
    eprintln!(
        "umlbuild kernel: make {} (log: {})",
        args.join(" "),
        log_path.display()
    );
    tracing::info!(phase, jobs, "umlbuild kernel: make {}", args.join(" "));
    let mut cmd = Command::new("make");
    cmd.current_dir(source);
    cmd.arg(format!("O={}", build_dir.display()));
    cmd.arg(format!("-j{jobs}"));
    for a in args {
        cmd.arg(a);
    }
    // Pipe make output to a log file by default — kernel builds dump
    // thousands of `CC file.o` lines that drown any interactive shell.
    // The user sees the make invocation summary; errors are propagated
    // via the exit status (then we tail the log on failure for the
    // operator to see).  Set UMLBUILD_VERBOSE=1 to keep stdout/stderr
    // attached.
    let verbose = std::env::var_os("UMLBUILD_VERBOSE").is_some();
    let status = if verbose {
        cmd.status()
            .with_context(|| format!("spawn make for phase '{phase}'"))?
    } else {
        let log = std::fs::File::create(&log_path)
            .with_context(|| format!("create {}", log_path.display()))?;
        let log2 = log
            .try_clone()
            .with_context(|| format!("clone log fd for {}", log_path.display()))?;
        cmd.stdout(log).stderr(log2);
        cmd.status()
            .with_context(|| format!("spawn make for phase '{phase}'"))?
    };
    if !status.success() {
        // On failure, surface the last bit of the log so the operator
        // sees what broke without having to know the path.
        if !verbose {
            eprintln!("umlbuild kernel: make {phase} failed — last 30 log lines:");
            if let Ok(text) = std::fs::read_to_string(&log_path) {
                for line in text.lines().rev().take(30).collect::<Vec<_>>().iter().rev() {
                    eprintln!("    {line}");
                }
            }
        }
        bail!("make {phase} exited with {status}");
    }
    Ok(())
}

fn sha256_of_file(path: &std::path::Path) -> Result<String> {
    use sha2::{Digest, Sha256};
    let bytes =
        std::fs::read(path).with_context(|| format!("read {} for sha256", path.display()))?;
    let mut h = Sha256::new();
    h.update(&bytes);
    Ok(hex::encode(h.finalize()))
}
