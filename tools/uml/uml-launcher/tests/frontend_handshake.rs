// SPDX-License-Identifier: GPL-2.0
//
// Real vhost-user frontend handshake test (workstream C-10 v2).
//
// Drives an actual `vhost::vhost_user::Frontend` against a
// spawned `uml-launcher backend console --socket …`
// subprocess. This is the most honest smoke test in the crate:
// the backend is the binary we ship, launched through its CLI
// surface, with its seccomp filter applied, serving over a real
// Unix-domain socket. If the handshake breaks because the
// filter is too tight, because the daemon fails to bind, or
// because the backend advertises the wrong feature set, this
// test catches it before any guest kernel does.
//
// Why a subprocess: the backend's production `run()` applies a
// process-global seccomp filter before entering the event loop.
// An in-process test that called `run()` would `SECCOMP_RET_KILL_
// PROCESS` the cargo harness itself on the first post-apply
// syscall the harness makes. Running the backend out-of-process
// confines seccomp to the child and covers the real deployment
// shape.
//
// The `CARGO_BIN_EXE_uml-launcher` env var is set by cargo for
// integration tests in the same package — it points at the
// freshly-built binary. No separate build step needed.

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost::vhost_user::{Frontend, VhostUserFrontend};
use vhost::VhostBackend;
use virtio_bindings::bindings::virtio_config::VIRTIO_F_VERSION_1;
use virtio_bindings::bindings::virtio_ring::VIRTIO_RING_F_EVENT_IDX;

/// Path to the freshly-built `uml-launcher` binary. Cargo sets
/// this automatically for tests in the same package.
fn launcher_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_uml-launcher"))
}

/// Spin until the socket file exists or the deadline passes.
/// The backend's `Listener::new(path, true)` binds
/// synchronously, so the socket appears within ~milliseconds
/// in practice; 5 s is insurance for a loaded CI runner.
fn wait_for_socket(path: &Path) -> Result<(), String> {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if path.exists() {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(20));
    }
    Err(format!("socket never appeared at {}", path.display()))
}

/// Kill + reap @child on drop. Integration tests panic on assert
/// failures; without this, a failed assertion would leak the
/// subprocess and the next run's socket path collision would be
/// a mystery.
struct ChildGuard(Option<Child>);
impl Drop for ChildGuard {
    fn drop(&mut self) {
        if let Some(mut c) = self.0.take() {
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}

#[test]
fn frontend_completes_feature_negotiation() {
    let tmp = tempfile::tempdir().expect("tempdir");
    let socket = tmp.path().join("uml-console-handshake.sock");

    // Spawn the real binary. `--socket <path>` is the only
    // required arg for the console class. Redirect stdio to
    // /dev/null-ish streams so the test isn't racing the
    // subprocess's stdin reader thread on the harness's stdin.
    let child = Command::new(launcher_bin())
        .arg("backend")
        .arg("console")
        .arg("--socket")
        .arg(&socket)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn uml-launcher backend console");
    let mut guard = ChildGuard(Some(child));

    // Wait for the socket. If the backend crashed (bad seccomp
    // filter, etc.) the socket will never appear; surface the
    // child's stderr to make the failure actionable.
    if let Err(e) = wait_for_socket(&socket) {
        let mut c = guard.0.take().expect("child still alive");
        let _ = c.kill();
        let out = c.wait_with_output().ok();
        let stderr = out
            .as_ref()
            .map(|o| String::from_utf8_lossy(&o.stderr).into_owned())
            .unwrap_or_default();
        panic!("{e}\nsubprocess stderr:\n{stderr}");
    }

    // Drive a real vhost-user handshake. `max_queue_num=2`
    // matches the console backend's NUM_QUEUES.
    let mut frontend = Frontend::connect(&socket, 2).expect("frontend connect");
    frontend.set_owner().expect("set_owner");

    let virtio_features = frontend.get_features().expect("get_features");
    assert!(
        virtio_features & (1u64 << VIRTIO_F_VERSION_1) != 0,
        "backend should advertise VIRTIO_F_VERSION_1, got {:#x}",
        virtio_features
    );
    assert!(
        virtio_features & (1u64 << VIRTIO_RING_F_EVENT_IDX) != 0,
        "backend should advertise VIRTIO_RING_F_EVENT_IDX, got {:#x}",
        virtio_features
    );
    assert!(
        virtio_features & VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits() != 0,
        "backend should advertise PROTOCOL_FEATURES, got {:#x}",
        virtio_features
    );

    // PROTOCOL_FEATURES must be acked on the virtio side before
    // GET_PROTOCOL_FEATURES is defined. Ack it, then read back
    // the protocol bitmap.
    frontend
        .set_features(virtio_features & VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits())
        .expect("set_features (protocol bit only)");

    let pf = frontend
        .get_protocol_features()
        .expect("get_protocol_features");
    assert!(
        pf.contains(VhostUserProtocolFeatures::MQ),
        "console backend should advertise MQ, got {:?}",
        pf
    );
    assert!(
        pf.contains(VhostUserProtocolFeatures::REPLY_ACK),
        "console backend should advertise REPLY_ACK, got {:?}",
        pf
    );

    // Drop the frontend → socket closes → backend's `serve()`
    // observes Disconnected → subprocess exits cleanly.
    drop(frontend);

    // Wait for the child to exit on its own. If it doesn't
    // within a bounded window, the test fails — a subprocess
    // that doesn't exit on frontend disconnect is a bug the
    // test should catch (it would also leak daemons in
    // production under frontend restarts).
    let mut child = guard.0.take().expect("child still alive");
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                assert!(
                    status.success(),
                    "backend subprocess exited non-zero: {:?}",
                    status
                );
                return;
            }
            Ok(None) => {
                if Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    panic!("backend subprocess did not exit within 5s of frontend disconnect");
                }
                thread::sleep(Duration::from_millis(20));
            }
            Err(e) => panic!("try_wait: {e}"),
        }
    }
}
