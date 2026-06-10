// SPDX-License-Identifier: GPL-2.0
//
// umlbuild rootfs — populate a staging directory with a minimal
// Alpine or Debian-slim root filesystem + the /sbin/init template.
//
// v1 supports two bases:
//   - "alpine"       (default): download alpine-minirootfs tarball,
//                    extract, then apk-add the package set via
//                    `apk --root` (rootless when possible).
//   - "debian-slim": run `mmdebstrap --mode=unshare ...` for a
//                    fully-rootless bootstrap.
//
// In either case, we install a generated /sbin/init that reads
// sandbox.cmd=... from /proc/cmdline, mounts standard pseudo-fs,
// drops uid to sandbox_uid, exec's the command, and poweroffs.
// Mirrors umlctl/deploy.rs::render_init_script in shape.

use anyhow::{anyhow, bail, Context, Result};
use clap::Args;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::{paths, profile};

#[derive(Args, Debug)]
pub struct RootfsArgs {
    /// Profile name or path to a profile TOML.
    #[arg(long, value_name = "NAME_OR_PATH")]
    pub profile: String,

    /// Destination directory for the populated rootfs.  Defaults to
    /// $XDG_CACHE_HOME/uml-build/rootfs/<profile>/.
    #[arg(long, value_name = "DIR")]
    pub out: Option<PathBuf>,

    /// Force re-population of an existing directory (will rm -rf it
    /// first).  Default behavior is to refuse on non-empty.
    #[arg(long)]
    pub force: bool,
}

pub fn run(args: RootfsArgs) -> Result<()> {
    let prof = profile::resolve(&args.profile)?;
    let p = paths::Paths::resolve()?;

    let out = args.out.unwrap_or_else(|| p.rootfs_dir(&prof.profile.name));

    if out.is_dir() {
        let nonempty = std::fs::read_dir(&out)
            .map(|mut it| it.next().is_some())
            .unwrap_or(false);
        if nonempty {
            if args.force {
                tracing::warn!("rm -rf {}", out.display());
                std::fs::remove_dir_all(&out)
                    .with_context(|| format!("remove {}", out.display()))?;
            } else {
                bail!("{} is non-empty; pass --force to overwrite", out.display());
            }
        }
    }
    std::fs::create_dir_all(&out).with_context(|| format!("create {}", out.display()))?;

    match prof.rootfs.base.as_str() {
        "alpine" => populate_alpine(&prof, &p, &out)?,
        "debian-slim" => populate_debian(&prof, &out)?,
        other => bail!("unsupported rootfs.base = {other}"),
    }

    install_init(&prof, &out)?;

    // Write a sidecar manifest beside the rootfs dir (one level up).
    let manifest = out.with_extension("manifest.toml");
    let n_packages = match prof.rootfs.base.as_str() {
        "alpine" => prof
            .rootfs
            .alpine
            .as_ref()
            .map(|a| a.packages.len())
            .unwrap_or(0),
        "debian-slim" => prof
            .rootfs
            .debian
            .as_ref()
            .map(|d| d.packages.len())
            .unwrap_or(0),
        _ => 0,
    };
    let mtext = format!(
        "schema_version = 1\n\
         [rootfs]\n\
         profile      = \"{}\"\n\
         base         = \"{}\"\n\
         output       = \"{}\"\n\
         packages     = {}\n",
        prof.profile.name,
        prof.rootfs.base,
        out.display(),
        n_packages,
    );
    std::fs::write(&manifest, mtext)
        .with_context(|| format!("write manifest {}", manifest.display()))?;

    eprintln!(
        "umlbuild rootfs: {} populated ({} bytes)",
        out.display(),
        du_bytes(&out)?
    );
    Ok(())
}

// ----------------------------------------------------------------------
// Alpine path: download minirootfs tarball, extract, apk add packages.
// ----------------------------------------------------------------------

fn populate_alpine(prof: &profile::Profile, p: &paths::Paths, out: &Path) -> Result<()> {
    let alpine = prof
        .rootfs
        .alpine
        .as_ref()
        .ok_or_else(|| anyhow!("[rootfs.alpine] section missing"))?;

    let tarball = fetch_alpine_minirootfs(alpine, &p.dl_dir())?;
    tracing::info!("extracting {} into {}", tarball.display(), out.display());
    let status = Command::new("tar")
        .args([
            "-C",
            out.to_str().unwrap(),
            "-xzf",
            tarball.to_str().unwrap(),
        ])
        .status()
        .context("spawn tar")?;
    if !status.success() {
        bail!("tar extraction failed: {status}");
    }

    if !alpine.packages.is_empty() {
        apk_add(out, alpine)?;
    }

    Ok(())
}

/// Install Alpine packages into the just-extracted rootfs.  Strategy
/// (in order of preference):
///   1. Host has `apk` on PATH and supports `--root` — use it.
///   2. Use `bwrap` (bubblewrap) — most portable rootless sandbox; the
///      Alpine tarball ships its own apk binary so we just bind-bind
///      the rootfs at / and run /sbin/apk inside.  Works on hosts
///      where AppArmor restricts unprivileged user-ns clone.
///   3. Use `unshare -r chroot` — fallback if bwrap isn't available;
///      requires `kernel.apparmor_restrict_unprivileged_userns = 0`
///      and `kernel.unprivileged_userns_clone = 1`.
fn apk_add(rootfs: &Path, alpine: &profile::AlpineSpec) -> Result<()> {
    let major_minor = alpine
        .version
        .splitn(3, '.')
        .take(2)
        .collect::<Vec<_>>()
        .join(".");
    let main = format!(
        "{}/v{}/main",
        alpine.mirror.trim_end_matches('/'),
        major_minor
    );
    let community = main.replacen("/main", "/community", 1);

    // Strategy 1: host apk.
    if let Ok(apk_host) = which("apk") {
        tracing::info!(
            apk = %apk_host.display(),
            n_packages = alpine.packages.len(),
            "apk_add: using host apk with --root"
        );
        let mut cmd = Command::new(&apk_host);
        cmd.args([
            "--root",
            rootfs.to_str().unwrap(),
            "--no-cache",
            "--initdb",
            "--repository",
            &main,
            "--repository",
            &community,
            "add",
        ]);
        for pkg in &alpine.packages {
            cmd.arg(pkg);
        }
        let status = cmd.status().context("spawn host apk")?;
        if !status.success() {
            bail!("host apk exited with {status}");
        }
        return Ok(());
    }

    // The Alpine tarball ships its own apk binary; verify it's there.
    let rootfs_apk = rootfs.join("sbin/apk");
    if !rootfs_apk.is_file() {
        bail!(
            "{} not found; the Alpine minirootfs tarball did not ship apk",
            rootfs_apk.display()
        );
    }

    // Make sure the in-chroot apk has a repositories file pointing at
    // the version we pinned.
    let repos = rootfs.join("etc/apk/repositories");
    std::fs::create_dir_all(rootfs.join("etc/apk")).ok();
    std::fs::write(&repos, format!("{main}\n{community}\n"))
        .with_context(|| format!("write {}", repos.display()))?;

    // Strategy 2: bwrap (bubblewrap) — works on hosts where AppArmor
    // restricts kernel.apparmor_restrict_unprivileged_userns.
    if let Ok(bwrap) = which("bwrap") {
        tracing::info!(
            bwrap = %bwrap.display(),
            n_packages = alpine.packages.len(),
            "apk_add: using bwrap"
        );
        let mut cmd = Command::new(&bwrap);
        cmd.args([
            "--bind",
            rootfs.to_str().unwrap(),
            "/",
            "--proc",
            "/proc",
            "--dev",
            "/dev",
            "--tmpfs",
            "/tmp",
            "--ro-bind",
            "/etc/resolv.conf",
            "/etc/resolv.conf",
            "--unshare-pid",
            "--unshare-ipc",
            "--unshare-uts",
            "--die-with-parent",
            "/sbin/apk",
            "add",
            "--no-cache",
        ]);
        for pkg in &alpine.packages {
            cmd.arg(pkg);
        }
        let status = cmd.status().context("spawn bwrap+apk")?;
        if !status.success() {
            bail!("bwrap+apk exited with {status}");
        }
        return Ok(());
    }

    // Strategy 3: unshare -r chroot.  Requires user-ns to be permitted.
    let unshare = which("unshare").context(
        "no rootless backend available — install one of `apk-tools`, \
         `bubblewrap`, or `util-linux` (>= 2.31), or set rootfs.base = \
         \"debian-slim\".",
    )?;
    tracing::info!(
        unshare = %unshare.display(),
        n_packages = alpine.packages.len(),
        "apk_add: using unshare -r chroot (apparmor may block this on Ubuntu 23.10+)"
    );

    // Stage host's resolv.conf temporarily.
    let resolv = rootfs.join("etc/resolv.conf");
    let staged_resolv = if !resolv.exists() {
        std::fs::create_dir_all(rootfs.join("etc")).ok();
        let host = std::path::Path::new("/etc/resolv.conf");
        if host.is_file() {
            std::fs::copy(host, &resolv)
                .with_context(|| format!("stage resolv.conf -> {}", resolv.display()))?;
            true
        } else {
            false
        }
    } else {
        false
    };

    let mut cmd = Command::new(&unshare);
    cmd.args([
        "-r",
        "--mount",
        "--mount-proc",
        rootfs.join("proc").to_str().unwrap(),
        "chroot",
        rootfs.to_str().unwrap(),
        "/sbin/apk",
        "add",
        "--no-cache",
    ]);
    for pkg in &alpine.packages {
        cmd.arg(pkg);
    }
    let status = cmd.status().context("spawn unshare+chroot+apk")?;

    if staged_resolv {
        std::fs::remove_file(&resolv).ok();
    }

    if !status.success() {
        bail!("unshare+chroot apk exited with {status}");
    }
    Ok(())
}

/// Download the pinned Alpine minirootfs tarball into the dl cache,
/// verify its sha256 against the profile, and return its path.
fn fetch_alpine_minirootfs(alpine: &profile::AlpineSpec, dl_dir: &Path) -> Result<PathBuf> {
    let filename = format!(
        "alpine-minirootfs-{ver}-x86_64.tar.gz",
        ver = alpine.version
    );
    let dest = dl_dir.join(&filename);

    if dest.is_file() {
        let got = sha256_of_file(&dest)?;
        if got.eq_ignore_ascii_case(&alpine.sha256) {
            tracing::info!("cached tarball {} (sha256 verified)", dest.display());
            return Ok(dest);
        }
        tracing::warn!(
            "cached {} has sha256={}, profile expects {}; re-downloading",
            dest.display(),
            &got[..16],
            &alpine.sha256[..16.min(alpine.sha256.len())]
        );
        std::fs::remove_file(&dest).ok();
    }

    let major_minor = alpine
        .version
        .splitn(3, '.')
        .take(2)
        .collect::<Vec<_>>()
        .join(".");
    let url = format!(
        "{mirror}/v{mm}/releases/x86_64/{file}",
        mirror = alpine.mirror.trim_end_matches('/'),
        mm = major_minor,
        file = filename,
    );

    tracing::info!("downloading {}", url);
    let status = Command::new("curl")
        .args(["-fL", "--retry", "3", "-o", dest.to_str().unwrap(), &url])
        .status()
        .context("spawn curl")?;
    if !status.success() {
        bail!("curl exited with {status} for {url}");
    }

    let got = sha256_of_file(&dest)?;
    if !got.eq_ignore_ascii_case(&alpine.sha256) {
        std::fs::remove_file(&dest).ok();
        bail!(
            "sha256 mismatch for {url}: got {} expected {}",
            got,
            alpine.sha256
        );
    }
    Ok(dest)
}

// ----------------------------------------------------------------------
// Debian-slim path: mmdebstrap.
// ----------------------------------------------------------------------

fn populate_debian(prof: &profile::Profile, out: &Path) -> Result<()> {
    let mmdebstrap = which("mmdebstrap").context(
        "rootfs.base = \"debian-slim\" requires mmdebstrap on host \
         (apt install mmdebstrap)",
    )?;
    let dspec = prof.rootfs.debian.clone().unwrap_or_default();
    let variant = if dspec.variant.is_empty() {
        "minbase".to_string()
    } else {
        dspec.variant
    };
    let suite = if dspec.suite.is_empty() {
        "bookworm".to_string()
    } else {
        dspec.suite
    };

    let mut cmd = Command::new(&mmdebstrap);
    cmd.arg("--mode=unshare");
    cmd.arg(format!("--variant={variant}"));
    cmd.arg("--components=main");
    if !dspec.packages.is_empty() {
        cmd.arg(format!("--include={}", dspec.packages.join(",")));
    }
    cmd.arg(suite);
    cmd.arg(out);

    tracing::info!(
        "running {} (this can take a few minutes)",
        mmdebstrap.display()
    );
    let status = cmd.status().context("spawn mmdebstrap")?;
    if !status.success() {
        bail!("mmdebstrap exited with {status}");
    }
    Ok(())
}

impl Default for profile::DebianSpec {
    fn default() -> Self {
        Self {
            suite: "bookworm".to_string(),
            variant: "minbase".to_string(),
            packages: vec![],
        }
    }
}

// ----------------------------------------------------------------------
// /sbin/init template.
// ----------------------------------------------------------------------

fn install_init(prof: &profile::Profile, out: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;

    let init = render_init(prof);
    let target = out.join("sbin/init");
    std::fs::create_dir_all(out.join("sbin")).ok();
    // Alpine's minirootfs ships /sbin/init -> /bin/busybox as a symlink;
    // std::fs::write would follow it and try to overwrite busybox.
    // Unlink first.
    match std::fs::symlink_metadata(&target) {
        Ok(_) => {
            std::fs::remove_file(&target)
                .with_context(|| format!("unlink existing {}", target.display()))?;
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(e).with_context(|| format!("stat {}", target.display())),
    }
    std::fs::write(&target, init.as_bytes())
        .with_context(|| format!("write {}", target.display()))?;
    let mut perms = std::fs::metadata(&target)?.permissions();
    perms.set_mode(0o755);
    std::fs::set_permissions(&target, perms)?;

    // Bake the default sandbox command into /etc/sandbox.cmd.  The
    // init template reads this file rather than parsing /proc/cmdline
    // (which can't carry spaces).  Cmdline overrides remain possible
    // via sandbox.cmdfile=/path or by remounting + overwriting.
    let cmd_path = out.join("etc/sandbox.cmd");
    std::fs::create_dir_all(out.join("etc")).ok();
    let cmd = if prof.instance.sandbox_cmd.is_empty() {
        "/bin/sh\n".to_string()
    } else {
        format!("{}\n", prof.instance.sandbox_cmd)
    };
    std::fs::write(&cmd_path, cmd).with_context(|| format!("write {}", cmd_path.display()))?;

    // Bake the network plan into /etc/sandbox.net so /sbin/init can
    // bring up the NIC defensively (no-op if mode = "none").  Format
    // is a sourceable shell snippet — simpler than parsing TOML in ash.
    let net_path = out.join("etc/sandbox.net");
    let net = &prof.network;
    let dns_lines = net
        .dns
        .iter()
        .map(|s| format!("nameserver {s}"))
        .collect::<Vec<_>>()
        .join("\\n");
    let net_sh = format!(
        "# umlbuild-generated network plan; sourced by /sbin/init.\n\
         NET_MODE={mode}\n\
         NET_GUEST_DEV={guest_dev}\n\
         NET_GUEST_IP={guest_ip}\n\
         NET_GATEWAY={gw}\n\
         NET_DNS_RESOLV='{dns_lines}'\n",
        mode = net.mode,
        guest_dev = guest_dev_for_driver(&net.driver),
        guest_ip = net.guest_ip,
        gw = net.gateway,
        dns_lines = dns_lines,
    );
    std::fs::write(&net_path, net_sh).with_context(|| format!("write {}", net_path.display()))?;

    // Pre-create the /results mountpoint so the init's tmpfs mount has
    // somewhere to land.
    std::fs::create_dir_all(out.join("results")).ok();

    Ok(())
}

/// Map the profile's `network.driver` to the guest-visible netdev name.
/// vector v1 = `vec0`, vector v2 = `vec2.0`.
fn guest_dev_for_driver(driver: &str) -> &'static str {
    match driver {
        "vector2" => "vec2.0",
        _ => "vec0",
    }
}

/// The init template.  Mirrors umlctl/deploy.rs::render_init_script in
/// shape: mount pseudo-fs defensively, bring lo up, read the sandbox
/// command from /etc/sandbox.cmd (baked into the rootfs at build
/// time), run it, halt.
fn render_init(prof: &profile::Profile) -> String {
    let uid = prof.rootfs.sandbox_uid;
    format!(
        r#"#!/bin/sh
# umlbuild-generated /sbin/init for profile '{name}'.
# Boots, runs /etc/sandbox.cmd as a shell pipeline, powers off.
# No daemons, no getty, no shell login.

set +e

# Standard pseudo-filesystems.  Defensive: never fail boot on these.
mount -t proc     proc   /proc           2>/dev/null || true
mount -t sysfs    sysfs  /sys            2>/dev/null || true
mount -t devtmpfs devtmpfs /dev          2>/dev/null || true
mount -t devpts   devpts /dev/pts        2>/dev/null || true
mount -t tmpfs    tmpfs  /tmp            2>/dev/null || true
mount -t tmpfs    tmpfs  /run            2>/dev/null || true
# /results was pre-created in the rootfs by umlbuild; tmpfs-mount it
# so it's writable even on read-only roots.
mount -t tmpfs    tmpfs  /results        2>/dev/null || true

# Loopback up.  Best-effort; not all profiles include iproute2.
( ip link set lo up 2>/dev/null || ifconfig lo up 2>/dev/null ) || true

# Bring up the guest NIC if the profile baked a network plan.
if [ -r /etc/sandbox.net ]; then
    # shellcheck disable=SC1091
    . /etc/sandbox.net
    if [ "$NET_MODE" = "tap" ] && [ -n "$NET_GUEST_DEV" ]; then
        ip addr add "$NET_GUEST_IP" dev "$NET_GUEST_DEV" 2>/dev/null
        ip link set "$NET_GUEST_DEV" up 2>/dev/null
        ip route add default via "$NET_GATEWAY" 2>/dev/null
        printf '%b\n' "$NET_DNS_RESOLV" > /etc/resolv.conf
        echo "umlbuild-init: brought up $NET_GUEST_DEV ($NET_GUEST_IP via $NET_GATEWAY)"
    fi
fi

# Resolve the sandbox command, in priority order:
#   1. sandbox.cmdb64=<base64> on /proc/cmdline  (interactive shell verb)
#   2. sandbox.cmdfile=<path> on /proc/cmdline   (alt baked path)
#   3. /etc/sandbox.cmd                          (default, baked at build)
SANDBOX_CMD=""
CMDB64=$(awk -v RS=' ' \
    '/^sandbox\.cmdb64=/{{ sub(/^sandbox\.cmdb64=/, ""); print }}' \
    /proc/cmdline 2>/dev/null)
if [ -n "$CMDB64" ]; then
    SANDBOX_CMD=$(printf '%s' "$CMDB64" | base64 -d 2>/dev/null)
fi
if [ -z "$SANDBOX_CMD" ]; then
    CMDFILE=$(awk -v RS=' ' \
        '/^sandbox\.cmdfile=/{{ sub(/^sandbox\.cmdfile=/, ""); print }}' \
        /proc/cmdline 2>/dev/null)
    [ -z "$CMDFILE" ] && CMDFILE=/etc/sandbox.cmd
    if [ -r "$CMDFILE" ]; then
        SANDBOX_CMD=$(cat "$CMDFILE")
    else
        SANDBOX_CMD="/bin/sh"
    fi
fi

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root
export TERM=dumb
export SHELL=/bin/sh

echo "umlbuild-init: profile={name}"
echo "umlbuild-init: cmd: $SANDBOX_CMD"

# Interactive vs captured mode: when stdin is a TTY (shell verb), run
# the cmd attached so the user sees output as it streams + can type.
# Otherwise (batch/umlctl), redirect to /results/ so the host can
# scrape the output after boot.
RUN_AS=""
if [ "{uid}" -ne 0 ] && id sandbox >/dev/null 2>&1; then
    RUN_AS="su sandbox -s /bin/sh -c"
else
    RUN_AS="/bin/sh -c"
fi

if [ -t 0 ]; then
    # Interactive: stdio passes through to the host TTY.
    $RUN_AS "cd /tmp && $SANDBOX_CMD"
    RC=$?
    echo "umlbuild-init: ----- done rc=$RC -----"
else
    # Batch: capture stdio into /results/ for host-side scraping.
    $RUN_AS "cd /tmp && $SANDBOX_CMD" >/results/stdout 2>/results/stderr
    RC=$?
    echo "$RC" >/results/rc
    echo "umlbuild-init: ----- stdout -----"
    cat /results/stdout 2>/dev/null
    echo "umlbuild-init: ----- stderr -----"
    cat /results/stderr 2>/dev/null
    echo "umlbuild-init: ----- done rc=$RC -----"
fi

sync
poweroff -f 2>/dev/null
halt -f    2>/dev/null
exit "$RC"
"#,
        name = prof.profile.name,
        uid = uid,
    )
}

// ----------------------------------------------------------------------
// Tiny utilities (no separate file yet).
// ----------------------------------------------------------------------

fn which(prog: &str) -> Result<PathBuf> {
    let path = std::env::var_os("PATH").context("PATH unset")?;
    for dir in std::env::split_paths(&path) {
        let cand = dir.join(prog);
        if cand.is_file() {
            return Ok(cand);
        }
    }
    Err(anyhow!("'{prog}' not found in PATH"))
}

fn sha256_of_file(path: &Path) -> Result<String> {
    use sha2::{Digest, Sha256};
    let bytes =
        std::fs::read(path).with_context(|| format!("read {} for sha256", path.display()))?;
    let mut h = Sha256::new();
    h.update(&bytes);
    Ok(hex::encode(h.finalize()))
}

fn du_bytes(dir: &Path) -> Result<u64> {
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
