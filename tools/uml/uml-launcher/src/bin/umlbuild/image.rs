// SPDX-License-Identifier: GPL-2.0
//
// umlbuild image — pack a populated rootfs directory into a
// ubd-attachable ext4 image without sudo, using `mkfs.ext4 -d`.
//
// The `-d <directory>` flag of mke2fs (e2fsprogs >= 1.43) populates the
// filesystem at creation time from a host directory tree, bypassing any
// need for a loop mount or a chroot.  This is the linchpin that makes
// umlbuild work in CI / unprivileged sandboxes.

use anyhow::{bail, Context, Result};
use clap::Args;
use std::path::PathBuf;
use std::process::Command;

#[derive(Args, Debug)]
pub struct ImageArgs {
    /// Source rootfs directory (output of `umlbuild rootfs`).
    #[arg(long, value_name = "DIR")]
    pub rootfs_dir: PathBuf,

    /// Destination image file (will be created/overwritten).
    #[arg(long, value_name = "PATH")]
    pub out: PathBuf,

    /// Image size as accepted by truncate(1): "200M", "2G", etc.
    #[arg(long, value_name = "SIZE", default_value = "200M")]
    pub size: String,

    /// Filesystem type.  Only "ext4" is supported at v1.
    #[arg(long, value_name = "FS", default_value = "ext4")]
    pub fstype: String,

    /// Filesystem label.
    #[arg(long, value_name = "LABEL", default_value = "uml-sandbox")]
    pub label: String,

    /// Skip the e2fsck self-check after creation.
    #[arg(long)]
    pub no_fsck: bool,
}

pub fn run(args: ImageArgs) -> Result<()> {
    if !args.rootfs_dir.is_dir() {
        bail!(
            "rootfs dir {} is missing or not a directory",
            args.rootfs_dir.display()
        );
    }
    if args.fstype != "ext4" {
        bail!("only fstype = ext4 is supported at v1 (got {})", args.fstype);
    }

    let bytes = parse_size(&args.size)?;
    let rootfs_bytes = du_bytes(&args.rootfs_dir)?;
    let min_bytes = (rootfs_bytes * 5 / 4).max(16 * 1024 * 1024); // +25% slack
    if bytes < min_bytes {
        bail!(
            "image size {} bytes is too small for rootfs {} bytes \
             (need >= {} bytes incl. 25% slack)",
            bytes,
            rootfs_bytes,
            min_bytes
        );
    }

    // Make sure the parent dir exists.
    if let Some(parent) = args.out.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create {}", parent.display()))?;
    }
    // Truncate the file to the requested size (sparse).
    let f = std::fs::File::create(&args.out)
        .with_context(|| format!("create {}", args.out.display()))?;
    f.set_len(bytes)
        .with_context(|| format!("truncate {} to {} bytes", args.out.display(), bytes))?;
    drop(f);

    // mke2fs -F -L $label -d $rootfs_dir -t ext4 $out
    tracing::info!(
        size_bytes = bytes,
        rootfs_dir = %args.rootfs_dir.display(),
        out = %args.out.display(),
        "running mkfs.ext4 -d"
    );
    let status = Command::new("mkfs.ext4")
        .args([
            "-F",
            "-L",
            &args.label,
            "-d",
            args.rootfs_dir.to_str().unwrap(),
            args.out.to_str().unwrap(),
        ])
        .status()
        .context("spawn mkfs.ext4 (install e2fsprogs >= 1.43)")?;
    if !status.success() {
        bail!("mkfs.ext4 exited with {status}");
    }

    if !args.no_fsck {
        tracing::info!("running e2fsck -fn for self-check");
        let status = Command::new("e2fsck")
            .args(["-fn", args.out.to_str().unwrap()])
            .status()
            .context("spawn e2fsck")?;
        if !status.success() {
            bail!("e2fsck reported a problem: {status}");
        }
    }

    // Sidecar manifest.
    let manifest = args.out.with_extension("manifest.toml");
    let size_on_disk = std::fs::metadata(&args.out)?.len();
    let mtext = format!(
        "schema_version = 1\n\
         [image]\n\
         output         = \"{}\"\n\
         rootfs_dir     = \"{}\"\n\
         size_bytes     = {}\n\
         rootfs_bytes   = {}\n\
         fstype         = \"{}\"\n\
         label          = \"{}\"\n",
        args.out.display(),
        args.rootfs_dir.display(),
        size_on_disk,
        rootfs_bytes,
        args.fstype,
        args.label,
    );
    std::fs::write(&manifest, mtext)
        .with_context(|| format!("write manifest {}", manifest.display()))?;

    eprintln!(
        "umlbuild image: {} ({} bytes, label={})",
        args.out.display(),
        size_on_disk,
        args.label,
    );
    Ok(())
}

fn parse_size(s: &str) -> Result<u64> {
    let s = s.trim();
    if s.is_empty() {
        bail!("empty size");
    }
    let (num_part, suf): (&str, u64) = match s.chars().last().unwrap() {
        'K' | 'k' => (&s[..s.len() - 1], 1024),
        'M' | 'm' => (&s[..s.len() - 1], 1024 * 1024),
        'G' | 'g' => (&s[..s.len() - 1], 1024 * 1024 * 1024),
        c if c.is_ascii_digit() => (s, 1),
        other => bail!("unrecognized size suffix '{other}' in '{s}'"),
    };
    let n: u64 = num_part
        .parse()
        .with_context(|| format!("parse number in size '{s}'"))?;
    Ok(n * suf)
}

fn du_bytes(dir: &std::path::Path) -> Result<u64> {
    let mut total = 0u64;
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in std::fs::read_dir(&d).with_context(|| format!("readdir {}", d.display()))? {
            let entry = entry?;
            let ft = entry.file_type()?;
            if ft.is_dir() {
                stack.push(entry.path());
            } else if ft.is_file() {
                total += entry.metadata()?.len();
            }
        }
    }
    Ok(total)
}
