// SPDX-License-Identifier: GPL-2.0
//
// Orchestration of per-device vhost-user backends
// (workstream C-10 v2 commit 10).
//
// `uml-launcher run --virtio <class>[:<args>]` spawns the
// matching `uml-launcher backend <class> …` subprocess,
// binds a one-shot socket under /tmp/, and hands the UML
// kernel a `virtio_uml.device=<sock>:<virtio-id>` cmdline
// entry so the guest's virtio_uml driver connects on boot.
//
// Per-class `args`:
//   * console           no args
//   * net:<tap>         tap interface name (required)
//   * block:<path>[,ro] image path (required) + optional
//                       read-only flag
//
// Lifecycle:
//   1. Parent launcher parses `--virtio` specs into VirtioSpecs.
//   2. For each spec, fork+exec `uml-launcher backend <class>
//      --socket <path> [class args]`.
//   3. Wait for the socket file to appear (the backend only
//      creates it after it's ready to accept). Bounded wait
//      so we don't hang on a crashed backend.
//   4. Return a BackendProcess handle per spec, plus the UML
//      cmdline args to append.
//   5. The launcher stores the handles; when UML's .wait()
//      returns, or a signal arrives, the handles' Drop sends
//      SIGTERM then reaps, ensuring no orphan backends
//      outlive the UML kernel.
//
// Virtio-ID numbering:
//   console = 1, net = 2, block = 3. virtio_uml's cmdline
//   parser ties `device=<socket>:<id>` to the guest's
//   virtio_bus slot. Multiple instances of the same class
//   (e.g. two disks) increment the id.
//
// This is a first-pass orchestration. The more capable
// design (full enum of class args, multi-disk slot
// allocation, SIGCHLD-driven supervision) can layer on
// top without breaking the existing interface.

use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::thread::sleep;
use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};

/// Socket-existence poll interval. Backends bind their
/// listener as the first thing in `serve()`, so the file
/// shows up within ~milliseconds on a healthy runner.
const SOCKET_POLL_INTERVAL: Duration = Duration::from_millis(20);

/// How long to wait before giving up on a backend startup.
/// A slow CI runner under load can take a second or two to
/// dispatch + bind; 5 s is generous.
const SOCKET_POLL_TIMEOUT: Duration = Duration::from_secs(5);

/// Class-specific arguments parsed from `--virtio <class>:<args>`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VirtioClass {
    /// console backend — no extra arguments.
    Console,
    /// net backend — `tap` interface name.
    Net { tap: String },
    /// block backend — image path + read-only flag.
    Block { image: PathBuf, read_only: bool },
}

/// One parsed --virtio spec.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VirtioSpec {
    pub class: VirtioClass,
}

impl VirtioSpec {
    /// Stable virtio-id per class. Multiple instances of the
    /// same class are not yet supported; orchestrate() rejects
    /// a duplicate class in the `specs` list. That restriction
    /// can relax once the class-id allocator learns to hand
    /// out successive slots (follow-on).
    fn virtio_id(&self) -> u32 {
        match self.class {
            VirtioClass::Console => 1,
            VirtioClass::Net { .. } => 2,
            VirtioClass::Block { .. } => 3,
        }
    }

    fn class_name(&self) -> &'static str {
        match self.class {
            VirtioClass::Console => "console",
            VirtioClass::Net { .. } => "net",
            VirtioClass::Block { .. } => "block",
        }
    }

    /// Public wrapper for dry-run cmdline rendering.
    pub fn class_name_public(&self) -> &'static str {
        self.class_name()
    }

    /// Public wrapper for dry-run cmdline rendering.
    pub fn virtio_id_public(&self) -> u32 {
        self.virtio_id()
    }
}

/// Parse a single `--virtio` value, e.g. `"console"`,
/// `"net:tap0"`, `"block:/path/rootfs.img,ro"`.
pub fn parse_virtio_spec(raw: &str) -> Result<VirtioSpec> {
    let (class, rest) = match raw.split_once(':') {
        Some((c, r)) => (c, Some(r)),
        None => (raw, None),
    };
    let class = match class {
        "console" => {
            if rest.is_some() {
                return Err(anyhow!("virtio console class takes no args"));
            }
            VirtioClass::Console
        }
        "net" => {
            let tap = rest
                .ok_or_else(|| anyhow!("virtio net class requires :<tap-name>"))?
                .to_string();
            if tap.is_empty() {
                return Err(anyhow!("virtio net: empty tap name"));
            }
            VirtioClass::Net { tap }
        }
        "block" => {
            let rest = rest
                .ok_or_else(|| anyhow!("virtio block class requires :<image>[,ro]"))?;
            // Split on ',' once — the leading segment is the
            // image path, the (optional) trailing segment is
            // a comma-separated flag list. Today only `ro` is
            // recognized; unknown flags error so typos aren't
            // silent.
            let (image, flags) = match rest.split_once(',') {
                Some((img, f)) => (img, Some(f)),
                None => (rest, None),
            };
            if image.is_empty() {
                return Err(anyhow!("virtio block: empty image path"));
            }
            let mut read_only = false;
            if let Some(flags) = flags {
                for flag in flags.split(',') {
                    match flag.trim() {
                        "" => {}
                        "ro" => read_only = true,
                        other => {
                            return Err(anyhow!(
                                "virtio block: unknown flag {other:?} \
                                 (known: ro)"
                            ));
                        }
                    }
                }
            }
            VirtioClass::Block {
                image: PathBuf::from(image),
                read_only,
            }
        }
        other => {
            return Err(anyhow!(
                "virtio class {other:?} unknown (known: console, net, block)"
            ));
        }
    };
    Ok(VirtioSpec { class })
}

/// A spawned backend subprocess + the socket path it's
/// serving on. `Drop` sends SIGTERM + reaps so a panic in the
/// launcher's main thread doesn't leak backends.
#[derive(Debug)]
pub struct BackendProcess {
    pub socket: PathBuf,
    pub virtio_id: u32,
    pub class: &'static str,
    child: Option<Child>,
}

impl BackendProcess {
    /// The kernel cmdline fragment that wires this backend
    /// into the guest's virtio_uml bus.
    pub fn cmdline_fragment(&self) -> String {
        format!(
            "virtio_uml.device={}:{}",
            self.socket.display(),
            self.virtio_id
        )
    }

    /// Child pid, for logging. Not currently used by the
    /// launcher main path (tracing just shows class+socket),
    /// but useful for tests and future lifecycle-management
    /// code.
    #[allow(dead_code)]
    pub fn pid(&self) -> Option<u32> {
        self.child.as_ref().map(|c| c.id())
    }
}

impl Drop for BackendProcess {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() {
            // SIGTERM first; give the backend a short window
            // to run its normal shutdown (drop epoll, clear
            // SOCKET file). If that doesn't work, the kernel
            // will eventually reap on our exit.
            let pid = child.id();
            // SAFETY: kill(2) with a live pid + SIGTERM.
            let _ = unsafe {
                libc::kill(pid as libc::pid_t, libc::SIGTERM)
            };
            // Short join; don't block shutdown on a
            // malfunctioning backend.
            let deadline = Instant::now() + Duration::from_millis(500);
            loop {
                match child.try_wait() {
                    Ok(Some(_)) => break,
                    Ok(None) if Instant::now() < deadline => {
                        sleep(Duration::from_millis(20));
                    }
                    _ => {
                        let _ = child.kill();
                        let _ = child.wait();
                        break;
                    }
                }
            }
            // Remove the socket file if the backend didn't.
            // Best-effort — may already be gone.
            let _ = std::fs::remove_file(&self.socket);
        }
    }
}

/// Orchestrator launcher input: the self-path of the
/// `uml-launcher` binary (so we invoke ourselves for the
/// `backend <class>` subcommand), the parsed specs, and the
/// pid of the parent for unique socket naming.
pub struct OrchestrateArgs {
    pub launcher_bin: PathBuf,
    pub specs: Vec<VirtioSpec>,
    /// Socket-path directory. /tmp on most hosts; tests pass
    /// a tempdir.
    pub socket_dir: PathBuf,
}

/// Spawn each backend, wait for its socket, return the
/// BackendProcess handles + the cmdline args to append to
/// the UML kernel invocation.
///
/// Errors mid-orchestrate: any previously-spawned backends
/// are torn down via BackendProcess's Drop on the Err path.
/// The caller sees a clean "no half-up state" failure.
pub fn orchestrate(
    args: OrchestrateArgs,
) -> Result<(Vec<BackendProcess>, Vec<String>)> {
    // Detect duplicate classes up front; the virtio-id
    // allocator is a stable per-class constant today and
    // doesn't yet multiplex. Surface the limitation
    // loudly instead of silently colliding.
    {
        use std::collections::HashSet;
        let mut seen = HashSet::new();
        for spec in &args.specs {
            let name = spec.class_name();
            if !seen.insert(name) {
                return Err(anyhow!(
                    "multiple --virtio {name} specs not yet supported; \
                     use a single instance per class"
                ));
            }
        }
    }

    let parent_pid = std::process::id();
    let mut processes: Vec<BackendProcess> = Vec::with_capacity(args.specs.len());
    let mut cmdline: Vec<String> = Vec::with_capacity(args.specs.len());

    for spec in args.specs {
        let class = spec.class_name();
        let sock = args
            .socket_dir
            .join(format!("uml-{parent_pid}-{class}.sock"));

        // Clean any stale socket from a prior crashed run.
        let _ = std::fs::remove_file(&sock);

        let mut cmd = Command::new(&args.launcher_bin);
        cmd.arg("backend").arg(class).arg("--socket").arg(&sock);
        match &spec.class {
            VirtioClass::Console => {}
            VirtioClass::Net { tap } => {
                cmd.arg("--tap").arg(tap);
            }
            VirtioClass::Block { image, read_only } => {
                cmd.arg("--image").arg(image);
                if *read_only {
                    cmd.arg("--read-only");
                }
            }
        }

        tracing::info!(
            class = class,
            socket = %sock.display(),
            "spawning backend"
        );
        let child = cmd
            .spawn()
            .with_context(|| format!("spawning {class} backend"))?;
        let virtio_id = spec.virtio_id();
        let mut handle = BackendProcess {
            socket: sock.clone(),
            virtio_id,
            class,
            child: Some(child),
        };

        if let Err(e) = wait_for_socket(&sock) {
            // Don't leak this backend on failure.
            drop(handle);
            return Err(e.context(format!(
                "{class} backend never created its socket at {}",
                sock.display()
            )));
        }

        cmdline.push(handle.cmdline_fragment());
        // Discard unused assignments to appease clippy under
        // the current code path — the handle's Drop is what
        // reaps if something further down fails.
        if handle.child.is_some() {
            processes.push(BackendProcess {
                socket: handle.socket.clone(),
                virtio_id: handle.virtio_id,
                class: handle.class,
                child: handle.child.take(),
            });
            // Disarm the original handle so its Drop doesn't
            // reap the still-running child.
            handle.child = None;
            let _ = handle;
        }
    }

    Ok((processes, cmdline))
}

/// Sanity helper: return Ok when the socket path exists, Err
/// once the deadline passes.
fn wait_for_socket(path: &Path) -> Result<()> {
    let deadline = Instant::now() + SOCKET_POLL_TIMEOUT;
    while Instant::now() < deadline {
        if path.exists() {
            return Ok(());
        }
        sleep(SOCKET_POLL_INTERVAL);
    }
    Err(anyhow!("socket {} did not appear within {:?}", path.display(), SOCKET_POLL_TIMEOUT))
}

/// Silence `dead_code` on AsRawFd which we import for
/// future libselinux/apparmor parallel wire-up without
/// currently using it in this module.
#[allow(dead_code)]
fn _unused() -> i32 {
    std::io::stderr().as_raw_fd()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_console_bare() {
        let s = parse_virtio_spec("console").unwrap();
        assert_eq!(s.class, VirtioClass::Console);
        assert_eq!(s.virtio_id(), 1);
        assert_eq!(s.class_name(), "console");
    }

    #[test]
    fn parse_console_rejects_args() {
        assert!(parse_virtio_spec("console:foo").is_err());
    }

    #[test]
    fn parse_net_requires_tap() {
        assert!(parse_virtio_spec("net").is_err());
        let s = parse_virtio_spec("net:tap0").unwrap();
        assert_eq!(s.class, VirtioClass::Net { tap: "tap0".into() });
        assert_eq!(s.virtio_id(), 2);
    }

    #[test]
    fn parse_net_empty_tap_errors() {
        assert!(parse_virtio_spec("net:").is_err());
    }

    #[test]
    fn parse_block_image_only() {
        let s = parse_virtio_spec("block:/tmp/rootfs.img").unwrap();
        assert_eq!(
            s.class,
            VirtioClass::Block {
                image: PathBuf::from("/tmp/rootfs.img"),
                read_only: false,
            }
        );
        assert_eq!(s.virtio_id(), 3);
    }

    #[test]
    fn parse_block_ro_flag() {
        let s = parse_virtio_spec("block:/tmp/x.img,ro").unwrap();
        assert!(matches!(
            s.class,
            VirtioClass::Block { read_only: true, .. }
        ));
    }

    #[test]
    fn parse_block_rejects_unknown_flag() {
        assert!(parse_virtio_spec("block:/tmp/x.img,weird").is_err());
    }

    #[test]
    fn parse_block_empty_image_errors() {
        assert!(parse_virtio_spec("block:").is_err());
        assert!(parse_virtio_spec("block:,ro").is_err());
    }

    #[test]
    fn parse_unknown_class_errors() {
        assert!(parse_virtio_spec("foo").is_err());
        assert!(parse_virtio_spec("foo:bar").is_err());
    }

    #[test]
    fn cmdline_fragment_shape() {
        let bp = BackendProcess {
            socket: PathBuf::from("/tmp/uml-1-console.sock"),
            virtio_id: 1,
            class: "console",
            child: None,
        };
        assert_eq!(
            bp.cmdline_fragment(),
            "virtio_uml.device=/tmp/uml-1-console.sock:1"
        );
    }

    #[test]
    fn orchestrate_rejects_duplicate_classes() {
        let args = OrchestrateArgs {
            launcher_bin: PathBuf::from("/nonexistent"),
            specs: vec![
                parse_virtio_spec("block:/a.img").unwrap(),
                parse_virtio_spec("block:/b.img").unwrap(),
            ],
            socket_dir: PathBuf::from("/tmp"),
        };
        let err = orchestrate(args).unwrap_err();
        assert!(err.to_string().contains("not yet supported"));
    }

    #[test]
    fn orchestrate_with_no_specs_is_noop() {
        let args = OrchestrateArgs {
            launcher_bin: PathBuf::from("/nonexistent"),
            specs: vec![],
            socket_dir: PathBuf::from("/tmp"),
        };
        let (procs, cmdline) = orchestrate(args).unwrap();
        assert!(procs.is_empty());
        assert!(cmdline.is_empty());
    }
}
